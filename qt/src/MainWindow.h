// ╔════════════════════════════════════════════════════════════════════════════════╗
// ║   MainWindow.h                                                                 ║
// ╚════════════════════════════════════════════════════════════════════════════════╝
//
// Layout:
//   connection bar   port / baud / connect / live status / options menu
//   left column      live readout (dBm, watts, level bar, peak hold, noise-floor tare),
//                    statistics, meter settings, threshold events, CSV log
//   right tabs       Monitor (rolling level chart), Sweep (scope view of one sweep with
//                    markers and trigger), Log (command console)
//
// A 50 ms UI timer drains the link's sample bin and fans the data out to the readout,
// statistics, chart, threshold events and CSV log. Sweeps are drawn as they arrive.

#pragma once

#include "MeterLink.h"
#include "TimeSeriesChart.h"

#include <QElapsedTimer>
#include <QFile>
#include <QMainWindow>
#include <QPointer>
#include <QTextStream>
#include <QTimer>

#include <deque>
#include <limits>
#include <optional>

class GlowReadout;
class QAction;
class QActionGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QToolButton;
class SignalBar;
class StatusDot;
class SweepScope;

struct StartupOptions
{
    QString port;
    bool connect = false;
    bool simulate = false;
    QSize size;
    QString screenshotPath;   // grab the window to this file after a few seconds, then quit
    int startTab = -1;   // --tab: open on this tab
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(const StartupOptions& options, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    // ── construction ──
    QWidget* buildConnectionBar();
    QWidget* buildLeftColumn();
    QWidget* buildReadoutCard();
    QWidget* buildStatsCard();
    QWidget* buildMeterCard();
    QWidget* buildEventsCard();
    QWidget* buildCsvCard();
    QWidget* buildMonitorTab();
    QWidget* buildSweepTab();
    QWidget* buildLogTab();
    void installShortcuts();

    // ── connection ──
    void refreshPorts(bool quiet);
    void toggleConnection();
    void connectSelected();
    void connectSimulator();
    void onConnectionChanged(bool open, const QString& reason);
    void watchPorts();
    void updateConnectionUi();
    QString selectedPort() const;
    int selectedBaud() const;

    // ── meter settings ──
    void onSettingsReceived(int frequencyMhz, double offsetDb);
    void applyToMeter();
    void setTimebaseIndex(int index, bool sendToMeter);
    void showMeterStatus(const QString& text, const QColor& color);

    // ── live data ──
    void onUiTick();
    void onSweep(const QVector<float>& sweep);
    void updateReadoutIdle();
    void resetPeak();
    void resetStats();
    void beginFloorCapture(bool openEnded);
    void finalizeFloorCapture();
    void clearFloor();
    void updateFloorReadout(const SampleBin& bin, double avg);
    void processThresholdEvent(double avg);

    // ── sweep view ──
    void setSweepRunning(bool running);
    void armSingle();
    void updateMarkerReadouts();
    void updateSweepBadge();
    void syncViewControls();
    void exportSweep();
    void saveImage();

    // ── log / CSV ──
    void appendLog(int level, const QString& text);
    void toggleCsvLog();
    void stopCsvLog();
    void openLogFolder();

    // ── settings ──
    void loadSettings();
    void saveSettings();

    StartupOptions options_;
    MeterLink link_;
    QTimer uiTimer_;
    QTimer portWatchTimer_;
    QTimer kDebounce_;
    QElapsedTimer clock_;

    // connection bar
    QLabel* brandTag_ = nullptr;
    QComboBox* portCombo_ = nullptr;
    QPushButton* rescanButton_ = nullptr;
    QMenu* baudMenu_ = nullptr;
    QActionGroup* baudGroup_ = nullptr;
    QPushButton* connectButton_ = nullptr;
    StatusDot* statusDot_ = nullptr;
    QLabel* statusText_ = nullptr;
    QToolButton* optionsButton_ = nullptr;
    QAction* actDtr_ = nullptr;
    QAction* actAutoConnect_ = nullptr;
    QAction* actAutoReconnect_ = nullptr;

    // readout
    GlowReadout* dbmReadout_ = nullptr;
    QLabel* wattsLabel_ = nullptr;
    SignalBar* signalBar_ = nullptr;
    QLabel* peakLabel_ = nullptr;
    QLabel* floorLabel_ = nullptr;
    QLabel* floorDeltaLabel_ = nullptr;
    QLabel* overloadLabel_ = nullptr;
    QPushButton* floorCaptureButton_ = nullptr;
    QPushButton* floorClearButton_ = nullptr;

    // statistics
    QLabel* statMin_ = nullptr;
    QLabel* statMax_ = nullptr;
    QLabel* statAvg_ = nullptr;
    QLabel* statCount_ = nullptr;

    // meter settings
    QLabel* meterReports_ = nullptr;
    QSpinBox* freqSpin_ = nullptr;
    QDoubleSpinBox* offsetSpin_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* readButton_ = nullptr;
    QComboBox* timebaseCombo_ = nullptr;
    QCheckBox* restoreCheck_ = nullptr;
    QLabel* meterStatus_ = nullptr;

    // threshold events
    QCheckBox* eventsEnable_ = nullptr;
    QDoubleSpinBox* eventsThreshold_ = nullptr;
    QListWidget* eventsList_ = nullptr;
    QLabel* eventsStatus_ = nullptr;

    // CSV log
    QPushButton* csvButton_ = nullptr;
    QLabel* csvStatus_ = nullptr;

    // tabs
    QTabWidget* tabs_ = nullptr;
    QComboBox* windowCombo_ = nullptr;
    QCheckBox* maxTraceCheck_ = nullptr;
    TimeSeriesChart* chart_ = nullptr;

    QPushButton* runButton_ = nullptr;
    QPushButton* singleButton_ = nullptr;
    QComboBox* sweepTimebaseCombo_ = nullptr;
    QComboBox* trigModeCombo_ = nullptr;
    QComboBox* trigEdgeCombo_ = nullptr;
    QDoubleSpinBox* trigLevelSpin_ = nullptr;
    QComboBox* scaleCombo_ = nullptr;
    QDoubleSpinBox* topSpin_ = nullptr;
    SweepScope* scope_ = nullptr;
    QLabel* m1Label_ = nullptr;
    QLabel* m2Label_ = nullptr;
    QLabel* deltaLabel_ = nullptr;
    QLabel* sweepStatsLabel_ = nullptr;

    QPlainTextEdit* logView_ = nullptr;
    QLabel* rawRecordLabel_ = nullptr;

    // ── state ──
    std::deque<ChartSample> chartBins_;
    double statMinValue_ = std::numeric_limits<double>::infinity();
    double statMaxValue_ = -std::numeric_limits<double>::infinity();
    double statSum_ = 0;
    qint64 statSamples_ = 0;
    double peakHold_ = -std::numeric_limits<double>::infinity();

    std::optional<int> meterFrequency_;
    bool boxesPrefilled_ = false;
    struct PendingSettings { int freq; double offset; };
    std::optional<PendingSettings> pendingSettings_;

    qint64 lastTotalRecords_ = 0;
    double lastRateAt_ = 0;
    double lastDataAt_ = 0;
    double currentRate_ = 0;

    double floorDbm_ = std::numeric_limits<double>::quiet_NaN();
    bool floorCapturing_ = false;
    double floorStartedAt_ = 0;
    std::optional<double> floorEndAt_;
    double floorSum_ = 0;
    qint64 floorCount_ = 0;

    bool eventArmed_ = true;
    int eventBinsAbove_ = 0;

    QFile csvFile_;
    QTextStream csvStream_;
    qint64 csvRows_ = 0;

    int timebaseIndex_ = 3;
    bool sweepRunning_ = true;
    qint64 sweepCount_ = 0;
    bool waitingForTrigger_ = false;

    QString reconnectPort_;           // set when the link was lost; cleared by user actions
    QStringList knownPorts_;
};
