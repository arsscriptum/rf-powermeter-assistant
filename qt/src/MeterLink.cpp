// ╔════════════════════════════════════════════════════════════════════════════════╗
// ║   MeterLink.cpp                                                                ║
// ╚════════════════════════════════════════════════════════════════════════════════╝

#include "MeterLink.h"

#include "MeterSimulator.h"

#include <algorithm>

namespace {
constexpr int kCommandSpacingMs = 300;     // the firmware needs a beat between commands
constexpr int kReadAfterSetMs = 1200;      // the stream pauses ~1 s after A / K
constexpr int kMinCompleteSweep = 450;     // shorter blocks are partial (just connected / K changed)
}

MeterLink::MeterLink(QObject* parent)
    : QObject(parent)
{
    clock_.start();
    pumpTimer_.setInterval(40);
    connect(&pumpTimer_, &QTimer::timeout, this, &MeterLink::pump);
}

MeterLink::~MeterLink()
{
    close();
}

bool MeterLink::isOpen() const
{
    return sim_ != nullptr || (serial_ && serial_->isOpen());
}

bool MeterLink::openSerial(const QString& portName, int baud, bool assertDtrRts, QString* error)
{
    close();
    auto* port = new QSerialPort(this);
    port->setPortName(portName);
    port->setBaudRate(baud);
    port->setDataBits(QSerialPort::Data8);
    port->setParity(QSerialPort::NoParity);
    port->setStopBits(QSerialPort::OneStop);
    port->setFlowControl(QSerialPort::NoFlowControl);
    if (!port->open(QIODevice::ReadWrite)) {
        if (error) *error = port->errorString();
        if (error && port->error() == QSerialPort::PermissionError) {
#ifdef Q_OS_LINUX
            *error += QStringLiteral(" (add your user to the 'dialout' group, or install the udev rule from packaging/linux)");
#else
            *error += QStringLiteral(" (is another program using the port?)");
#endif
        }
        delete port;
        return false;
    }
    // The STM32 VCP firmware variant only streams with DTR asserted on some hosts; the
    // USB-UART variant does not care. Configurable, asserted by default.
    port->setDataTerminalReady(assertDtrRts);
    port->setRequestToSend(assertDtrRts);
    port->clear(QSerialPort::Input);

    serial_ = port;
    portName_ = portName;
    baud_ = baud;
    connect(port, &QSerialPort::readyRead, this, [this] {
        if (serial_) handleBytes(serial_->readAll());
    });
    connect(port, &QSerialPort::errorOccurred, this, &MeterLink::onSerialError);
    startSession();
    emit logLine(LogInfo, QStringLiteral("Connected %1 @ %2").arg(portName).arg(baud));
    return true;
}

void MeterLink::openSimulator()
{
    close();
    sim_ = new MeterSimulator(this);
    portName_ = QStringLiteral("Simulator");
    baud_ = rfpm::kDefaultBaud;
    connect(sim_, &MeterSimulator::dataReady, this, &MeterLink::handleBytes);
    startSession();
    sim_->start();
    emit logLine(LogInfo, QStringLiteral("Connected to the built-in simulator (demo mode)"));
}

void MeterLink::startSession()
{
    parser_.reset();
    result_.clear();
    bin_ = SampleBin();
    lastRecord_.clear();
    skipNextBlock_ = true;   // the first block after opening is usually partial
    queue_.clear();
    lastScheduledMs_ = -1000000;
    sentK_ = 0;
    readDeadlineMs_ = -1;
    readRetries_ = 0;
    pumpTimer_.start();
    emit connectionChanged(true, QString());
}

void MeterLink::close(const QString& reason)
{
    const bool wasOpen = isOpen();
    pumpTimer_.stop();
    queue_.clear();
    readDeadlineMs_ = -1;
    if (serial_) {
        serial_->disconnect(this);
        if (serial_->isOpen()) serial_->close();
        serial_->deleteLater();
        serial_ = nullptr;
    }
    if (sim_) {
        sim_->stop();
        sim_->deleteLater();
        sim_ = nullptr;
    }
    if (wasOpen) {
        emit logLine(reason.isEmpty() ? LogInfo : LogError,
                     reason.isEmpty() ? QStringLiteral("Disconnected") : QStringLiteral("Disconnected: %1").arg(reason));
        emit connectionChanged(false, reason);
    }
}

void MeterLink::onSerialError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError || error == QSerialPort::TimeoutError) return;
    const bool fatal = error == QSerialPort::ResourceError || error == QSerialPort::DeviceNotFoundError
                       || error == QSerialPort::PermissionError || error == QSerialPort::ReadError;
    const QString text = serial_ ? serial_->errorString() : QStringLiteral("serial error %1").arg(int(error));
    if (!fatal) {
        emit logLine(LogError, text);
        if (serial_) serial_->clearError();
        return;
    }
    // Never delete the port from inside its own signal
    QTimer::singleShot(0, this, [this, text] { close(text.isEmpty() ? QStringLiteral("device lost") : text); });
}

SampleBin MeterLink::drainBin()
{
    SampleBin b = bin_;
    bin_ = SampleBin();
    return b;
}

void MeterLink::handleBytes(const QByteArray& bytes)
{
    if (bytes.isEmpty()) return;
    result_.clear();
    parser_.feed(bytes.constData(), static_cast<std::size_t>(bytes.size()), result_);

    for (float v : result_.samples) {
        ++bin_.count;
        bin_.sum += v;
        if (v < bin_.min) bin_.min = v;
        if (v > bin_.max) bin_.max = v;
        bin_.last = v;
    }
    if (!result_.lastRecord.empty()) lastRecord_ = QString::fromStdString(result_.lastRecord);

    for (const auto& s : result_.settings) {
        emit logLine(LogReceived, QStringLiteral("R-") + QString::fromStdString(s.raw));
        readDeadlineMs_ = -1;
        readRetries_ = 0;
        emit settingsReceived(s.frequencyMhz, s.offsetDb);
    }

    for (auto& block : result_.blocks) {
        if (skipNextBlock_) {
            skipNextBlock_ = false;
            continue;
        }
        if (static_cast<int>(block.size()) < kMinCompleteSweep) continue;
        QVector<float> sweep(static_cast<int>(block.size()));
        std::copy(block.begin(), block.end(), sweep.begin());
        emit sweepReceived(sweep);
    }
}

// ── command queue ───────────────────────────────────────────────────────────────

void MeterLink::enqueue(Kind kind, const QByteArray& bytes, int delayMs, int k)
{
    if (!isOpen()) return;
    // Only verified command shapes ever reach the queue (the short A form corrupts the meter)
    if (!rfpm::isSafeCommand(bytes.toStdString())) {
        emit logLine(LogError, QStringLiteral("refusing to send malformed command '%1'").arg(QString::fromLatin1(bytes).trimmed()));
        return;
    }
    // A newer rate replaces a rate that hasn't gone out yet
    if (kind == Kind::Rate) {
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [](const Pending& p) { return p.kind == Kind::Rate; }),
                     queue_.end());
    }
    const qint64 now = clock_.elapsed();
    qint64 due = std::max(now + delayMs, lastScheduledMs_ + kCommandSpacingMs);
    lastScheduledMs_ = due;
    queue_.push_back({kind, bytes, due, k});
    std::stable_sort(queue_.begin(), queue_.end(), [](const Pending& a, const Pending& b) { return a.dueMs < b.dueMs; });
}

void MeterLink::requestSettings(int delayMs)
{
    readRetries_ = 0;
    enqueue(Kind::Read, QByteArray::fromStdString(rfpm::readCommand()), delayMs);
}

bool MeterLink::applySettings(int frequencyMhz, double offsetDb, QString* error)
{
    const auto cmd = rfpm::setCommand(frequencyMhz, offsetDb);
    if (!cmd) {
        if (error) *error = QStringLiteral("frequency must be 1..9999 MHz and offset -99.9..+99.9 dB");
        return false;
    }
    if (!isOpen()) {
        if (error) *error = QStringLiteral("not connected");
        return false;
    }
    enqueue(Kind::Set, QByteArray::fromStdString(*cmd), 0);
    readRetries_ = 0;
    enqueue(Kind::Read, QByteArray::fromStdString(rfpm::readCommand()), kReadAfterSetMs);
    return true;
}

void MeterLink::setSampleRate(int k, int delayMs)
{
    const auto cmd = rfpm::sampleRateCommand(k);
    if (cmd) enqueue(Kind::Rate, QByteArray::fromStdString(*cmd), delayMs, k);
}

bool MeterLink::writeNow(const Pending& cmd)
{
    if (sim_) {
        sim_->write(cmd.bytes);
    } else if (serial_ && serial_->isOpen()) {
        // One atomic write: the meter's parser cannot resync mid-command
        if (serial_->write(cmd.bytes) != cmd.bytes.size()) {
            emit logLine(LogError, QStringLiteral("write failed: %1").arg(serial_->errorString()));
            return false;
        }
    } else {
        return false;
    }
    emit logLine(LogSent, QStringLiteral("S-") + QString::fromLatin1(cmd.bytes).trimmed());
    return true;
}

void MeterLink::pump()
{
    const qint64 now = clock_.elapsed();

    while (!queue_.empty() && queue_.front().dueMs <= now) {
        const Pending cmd = queue_.front();
        queue_.pop_front();
        if (!writeNow(cmd)) continue;
        switch (cmd.kind) {
        case Kind::Rate:
            sentK_ = cmd.k;
            skipNextBlock_ = true;   // the block in flight may still be at the old rate
            break;
        case Kind::Set:
            skipNextBlock_ = true;
            break;
        case Kind::Read: {
            // The reply rides on the next block marker: allow for a full sweep at slow rates
            const double sweep = rfpm::sweepDuration(sentK_ > 0 ? sentK_ : 1);
            readDeadlineMs_ = now + 2500 + static_cast<qint64>(sweep * 1200.0);
            break;
        }
        }
    }

    // The first Read after an A command is sometimes swallowed: retry once
    if (readDeadlineMs_ >= 0 && now >= readDeadlineMs_) {
        readDeadlineMs_ = -1;
        if (readRetries_ < 1) {
            ++readRetries_;
            enqueue(Kind::Read, QByteArray::fromStdString(rfpm::readCommand()), 0);
        } else {
            readRetries_ = 0;
            emit logLine(LogError, QStringLiteral("no reply to Read"));
            emit readFailed();
        }
    }
}
