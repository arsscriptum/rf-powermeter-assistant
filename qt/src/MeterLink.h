// ╔════════════════════════════════════════════════════════════════════════════════╗
// ║   MeterLink.h                                                                  ║
// ║   Owns the serial port (or the simulator), parses the stream, paces commands   ║
// ╚════════════════════════════════════════════════════════════════════════════════╝
//
// Runs on the UI thread: QSerialPort is asynchronous and the parser costs microseconds per
// chunk (the wire tops out at ~46 kB/s), so no worker thread is needed.
//
// Samples are accumulated into a "bin" that the UI drains on its own timer (avg/min/max per
// UI tick, like the C# companion). Complete 500-sample sweeps are signalled as they arrive.
//
// Commands go through a queue: the firmware parser is fragile, so every command is one
// write, commands are spaced >= 300 ms apart, and a Read that gets no reply is retried once.

#pragma once

#include "MeterProtocol.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QSerialPort>
#include <QTimer>
#include <QVector>

#include <deque>
#include <limits>

class MeterSimulator;

/// All dBm samples accumulated between two drainBin() calls.
struct SampleBin
{
    int count = 0;
    double sum = 0.0;
    double min = std::numeric_limits<double>::infinity();
    double max = -std::numeric_limits<double>::infinity();
    double last = std::numeric_limits<double>::quiet_NaN();

    double avg() const { return count > 0 ? sum / count : std::numeric_limits<double>::quiet_NaN(); }
};

class MeterLink : public QObject
{
    Q_OBJECT

public:
    enum LogLevel { LogInfo, LogSent, LogReceived, LogError };

    explicit MeterLink(QObject* parent = nullptr);
    ~MeterLink() override;

    bool openSerial(const QString& portName, int baud, bool assertDtrRts, QString* error);
    void openSimulator();
    /// Close the port. A non-empty reason means the link was lost (not a user action).
    void close(const QString& reason = QString());

    bool isOpen() const;
    bool isSimulated() const { return sim_ != nullptr; }
    QString portName() const { return portName_; }
    int baudRate() const { return baud_; }

    SampleBin drainBin();
    qint64 totalRecords() const { return parser_.totalRecords(); }
    qint64 malformedTokens() const { return parser_.malformedTokens(); }
    QString lastRecord() const { return lastRecord_; }
    /// The K command last written (0 = never set on this connection; the meter can't report it).
    int sentK() const { return sentK_; }

    // ── commands (queued) ──
    void requestSettings(int delayMs = 0);
    /// Queue "A<freq><offset>" followed by a verifying Read. False (and nothing sent) if invalid.
    bool applySettings(int frequencyMhz, double offsetDb, QString* error);
    void setSampleRate(int k, int delayMs = 0);

signals:
    void connectionChanged(bool open, const QString& reason);
    void sweepReceived(const QVector<float>& sweep);
    void settingsReceived(int frequencyMhz, double offsetDb);
    void readFailed();
    void logLine(int level, const QString& text);

private:
    enum class Kind { Read, Set, Rate };
    struct Pending
    {
        Kind kind;
        QByteArray bytes;
        qint64 dueMs;
        int k;
    };

    void startSession();
    void handleBytes(const QByteArray& bytes);
    void enqueue(Kind kind, const QByteArray& bytes, int delayMs, int k = 0);
    void pump();
    bool writeNow(const Pending& cmd);
    void onSerialError(QSerialPort::SerialPortError error);

    QPointer<QSerialPort> serial_;
    MeterSimulator* sim_ = nullptr;
    QString portName_;
    int baud_ = rfpm::kDefaultBaud;

    rfpm::StreamParser parser_;
    rfpm::ParseResult result_;
    SampleBin bin_;
    QString lastRecord_;
    bool skipNextBlock_ = false;

    QElapsedTimer clock_;
    QTimer pumpTimer_;
    std::deque<Pending> queue_;
    qint64 lastScheduledMs_ = -1000000;
    int sentK_ = 0;
    qint64 readDeadlineMs_ = -1;
    int readRetries_ = 0;
};
