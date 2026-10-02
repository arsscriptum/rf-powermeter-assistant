// ╔════════════════════════════════════════════════════════════════════════════════╗
// ║   MeterSimulator.cpp                                                           ║
// ╚════════════════════════════════════════════════════════════════════════════════╝

#include "MeterSimulator.h"

#include "MeterProtocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr double kWireSeconds = 0.158;   // time to ship one 500-sample block at 460800 baud
constexpr double kNoiseFloorDbm = -58.6;
constexpr double kPi = 3.14159265358979323846;
}

MeterSimulator::MeterSimulator(QObject* parent)
    : QObject(parent)
{
    timer_.setInterval(15);
    connect(&timer_, &QTimer::timeout, this, &MeterSimulator::tick);
}

void MeterSimulator::start()
{
    clock_.start();
    nextBlockAt_ = 0.05;
    pausedUntil_ = 0.0;
    timer_.start();
}

void MeterSimulator::stop()
{
    timer_.stop();
}

void MeterSimulator::write(const QByteArray& command)
{
    const double now = clock_.isValid() ? clock_.elapsed() / 1000.0 : 0.0;
    const std::string cmd = command.toStdString();
    if (!rfpm::isSafeCommand(cmd)) return;   // the real parser ignores what it does not understand

    if (cmd == "Read\r\n") {
        readPending_ = true;
    } else if (cmd[0] == 'K') {
        k_ = (cmd[1] - '0') * 10 + (cmd[2] - '0');
        pausedUntil_ = now + 1.0;
    } else if (cmd[0] == 'A') {
        frequencyMhz_ = std::stoi(cmd.substr(1, 4));
        offsetDb_ = std::stod(cmd.substr(5, 5));
        pausedUntil_ = now + 1.0;
    }
}

void MeterSimulator::tick()
{
    const double now = clock_.elapsed() / 1000.0;
    if (now < pausedUntil_) {
        nextBlockAt_ = std::max(nextBlockAt_, pausedUntil_);
        return;
    }
    if (now < nextBlockAt_) return;

    QByteArray block = makeBlock();
    nextBlockAt_ = now + rfpm::sweepDuration(k_) + kWireSeconds;

    // Ship it in uneven chunks, like a real USB link, so record splits get exercised.
    int pos = 0;
    int chunk = 997;
    while (pos < block.size()) {
        const int n = std::min(chunk, static_cast<int>(block.size()) - pos);
        emit dataReady(block.mid(pos, n));
        pos += n;
        chunk = chunk == 997 ? 1531 : 997;
    }
}

double MeterSimulator::signalAt(double t)
{
    // A pulsed carrier (1.22 kHz, 78 % duty) on top of the detector noise floor, with a slow
    // level drift and a strong burst every ~12 s, so both views have something to show.
    const double pulsePeriod = 1.0 / 1220.0;
    const double phase = std::fmod(t, pulsePeriod) / pulsePeriod;
    const bool carrierOn = phase < 0.78;

    const double cycle = std::fmod(t, 12.0);
    double carrier = -46.0 + 3.0 * std::sin(2 * kPi * t / 7.0);
    if (cycle > 8.0 && cycle < 9.6) carrier = -24.0 + 1.5 * std::sin(2 * kPi * t * 3.0);
    const bool carrierPresent = std::fmod(t, 30.0) < 24.0;   // off for 6 s of every 30 s

    // Incoherent power sum of carrier and floor
    double pw = rfpm::dbmToWatts(kNoiseFloorDbm);
    if (carrierPresent && carrierOn) pw += rfpm::dbmToWatts(carrier);
    double dbm = rfpm::wattsToDbm(pw) + noise_(rng_);

    // Band calibration: a small frequency-dependent tilt, like the real detector
    dbm += 0.4 * std::log10(std::max(1, frequencyMhz_)) - 1.2;
    return dbm + offsetDb_;
}

void MeterSimulator::appendRecord(QByteArray& out, double dbm)
{
    dbm = std::clamp(dbm, -99.9, 99.9);
    const int tenths = static_cast<int>(std::lround(std::fabs(dbm) * 10.0));
    const double w = rfpm::dbmToWatts(dbm);
    char unit = 'u';
    double v = w * 1e6;
    if (w >= 1.0) { unit = 'w'; v = w; }
    else if (w >= 1e-3) { unit = 'm'; v = w * 1e3; }
    const int hundredths = static_cast<int>(std::min(99999L, std::lround(v * 100.0)));
    char buf[16];
    std::snprintf(buf, sizeof buf, "%c%03d%05d%c", dbm < 0 ? '-' : '+', tenths, hundredths, unit);
    out.append(buf, 10);
}

QByteArray MeterSimulator::makeBlock()
{
    QByteArray out;
    out.reserve(rfpm::kSamplesPerBlock * 10 + 16);

    const double period = rfpm::samplePeriod(k_);
    for (int i = 0; i < rfpm::kSamplesPerBlock; ++i) appendRecord(out, signalAt(sweepTime_ + i * period));
    sweepTime_ += rfpm::sweepDuration(k_) + kWireSeconds;

    // Block marker, with the Read reply injected between 'A' and 'a'
    out.append('A');
    if (readPending_) {
        readPending_ = false;
        char buf[16];
        std::snprintf(buf, sizeof buf, "R%04d%s", frequencyMhz_, rfpm::formatOffset(offsetDb_).c_str());
        out.append(buf);
    }
    out.append('a');
    return out;
}
