// ╔════════════════════════════════════════════════════════════════════════════════╗
// ║   MeterSimulator.h                                                             ║
// ║   In-process stand-in for the meter: speaks the real wire format               ║
// ╚════════════════════════════════════════════════════════════════════════════════╝
//
// Produces the byte stream the hardware would (records, "Aa" block markers, "R" replies
// injected between 'A' and 'a') and obeys Read / A / K commands, including the ~1 s stream
// pause after a settings change. Everything goes through the normal parser, so demo mode
// exercises the same code path as a real meter.

#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <random>

class MeterSimulator : public QObject
{
    Q_OBJECT

public:
    explicit MeterSimulator(QObject* parent = nullptr);

    void start();
    void stop();
    /// Handle one command written by the host (same bytes as on the wire).
    void write(const QByteArray& command);

signals:
    void dataReady(const QByteArray& bytes);

private:
    void tick();
    QByteArray makeBlock();
    double signalAt(double t);
    static void appendRecord(QByteArray& out, double dbm);

    QTimer timer_;
    QElapsedTimer clock_;
    std::mt19937 rng_{12345};
    std::normal_distribution<double> noise_{0.0, 0.35};

    int k_ = 2;                  // boot default region
    int frequencyMhz_ = 1;       // power-on default: 1 MHz
    double offsetDb_ = 0.0;
    bool readPending_ = false;
    double nextBlockAt_ = 0.0;   // seconds on clock_
    double pausedUntil_ = 0.0;
    double sweepTime_ = 0.0;     // simulated acquisition time of the next sweep
};
