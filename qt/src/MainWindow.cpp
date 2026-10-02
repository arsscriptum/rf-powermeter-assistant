// ╔════════════════════════════════════════════════════════════════════════════════╗
// ║   MainWindow.cpp                                                               ║
// ╚════════════════════════════════════════════════════════════════════════════════╝

#include "MainWindow.h"

#include "MeterProtocol.h"
#include "SweepScope.h"
#include "Theme.h"
#include "Widgets.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QActionGroup>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSerialPortInfo>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kUiTickMs = 50;
constexpr std::size_t kMaxChartBins = 15 * 60 * (1000 / kUiTickMs) + 200;   // 15 min of history
constexpr double kOverloadDbm = 5.0;       // AD8317 compresses above ~-5 dBm; damage at ~+12 dBm

const int kChartWindows[] = {10, 30, 60, 300, 900};
const char* const kChartWindowLabels[] = {"10 s", "30 s", "60 s", "5 min", "15 min"};

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

QString dbmText(double v, int decimals = 1)
{
    return qs(rfpm::formatDbm(v, decimals));
}

QString wattsText(double w)
{
    return qs(rfpm::formatWatts(w));
}

QString css(const QColor& c)
{
    return c.name(QColor::HexRgb);
}

QLabel* makeLabel(const QString& text, const char* role = nullptr)
{
    auto* l = new QLabel(text);
    if (role) l->setProperty("role", QString::fromLatin1(role));
    return l;
}

/// Small caps purple header at the top of a card.
QLabel* sectionHeader(const QString& text)
{
    auto* l = makeLabel(text.toUpper(), "section");
    QFont f = theme::uiFont(8, QFont::DemiBold);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
    l->setFont(f);
    return l;
}

/// Tiny caption ("PORT", "WINDOW") in front of a toolbar control.
QLabel* caption(const QString& text)
{
    auto* l = makeLabel(text.toUpper(), "caption");
    QFont f = theme::uiFont(7.5, QFont::DemiBold);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
    l->setFont(f);
    return l;
}

QFrame* makeCard(QVBoxLayout** layoutOut, const char* role = "card")
{
    auto* frame = new QFrame;
    theme::makeCard(frame, role);
    auto* v = new QVBoxLayout(frame);
    v->setContentsMargins(14, 12, 14, 14);
    v->setSpacing(8);
    *layoutOut = v;
    return frame;
}

QPushButton* makeButton(const QString& text, const QString& tip = QString())
{
    auto* b = new QPushButton(text);
    b->setCursor(Qt::PointingHandCursor);
    if (!tip.isEmpty()) b->setToolTip(tip);
    return b;
}

/// Keeps a caption and its controls together when a flow toolbar wraps.
QWidget* group(std::initializer_list<QWidget*> widgets)
{
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(6);
    for (QWidget* child : widgets) h->addWidget(child);
    return w;
}

void setColor(QLabel* l, const QColor& c)
{
    l->setStyleSheet(QStringLiteral("color: %1;").arg(css(c)));
}

/// Ports worth listing. On Linux, legacy UARTs (ttyS*) without a USB id are just noise.
bool listedPort(const QSerialPortInfo& info)
{
#ifdef Q_OS_LINUX
    if (info.portName().startsWith(QLatin1String("ttyS")) && !info.hasVendorIdentifier()) return false;
#else
    Q_UNUSED(info);
#endif
    return true;
}

QString documentsDir(const QString& sub)
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString dir = QDir(base).filePath(QStringLiteral("rfmeter/") + sub);
    QDir().mkpath(dir);
    return dir;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════════
//  construction
// ═════════════════════════════════════════════════════════════════════════════════

MainWindow::MainWindow(const StartupOptions& options, QWidget* parent)
    : QMainWindow(parent)
    , options_(options)
{
    clock_.start();
    setWindowTitle(QStringLiteral("RF Power Meter V5 — Assistant"));
    setWindowIcon(theme::appIcon());
    setMinimumSize(980, 640);
    resize(1280, 820);

    auto* central = new QWidget;
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(16, 12, 16, 16);
    root->setSpacing(12);
    root->addWidget(buildConnectionBar());

    auto* body = new QHBoxLayout;
    body->setSpacing(14);
    body->addWidget(buildLeftColumn());

    tabs_ = new QTabWidget;
    tabs_->setDocumentMode(true);
    tabs_->tabBar()->setDrawBase(false);
    tabs_->addTab(buildMonitorTab(), QStringLiteral("Monitor"));
    tabs_->addTab(buildSweepTab(), QStringLiteral("Sweep"));
    tabs_->addTab(buildLogTab(), QStringLiteral("Log"));
    tabs_->setTabToolTip(0, QStringLiteral("Rolling power level: find and track a signal (Ctrl+1)"));
    tabs_->setTabToolTip(1, QStringLiteral("One 500-sample sweep, oscilloscope style: pulses, timing, markers (Ctrl+2)"));
    tabs_->setTabToolTip(2, QStringLiteral("Commands sent to the meter and its replies (Ctrl+3)"));
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int i) {
        if (i == 2) tabs_->setTabText(2, QStringLiteral("Log"));
    });
    body->addWidget(tabs_, 1);
    root->addLayout(body, 1);
    setCentralWidget(central);

    // ── link ──
    connect(&link_, &MeterLink::connectionChanged, this, &MainWindow::onConnectionChanged);
    connect(&link_, &MeterLink::sweepReceived, this, &MainWindow::onSweep);
    connect(&link_, &MeterLink::settingsReceived, this, &MainWindow::onSettingsReceived);
    connect(&link_, &MeterLink::logLine, this, &MainWindow::appendLog);
    connect(&link_, &MeterLink::readFailed, this, [this] {
        if (pendingSettings_) {
            pendingSettings_.reset();
            showMeterStatus(QStringLiteral("no reply from the meter: click Read back to verify"), theme::Warn);
        }
    });

    // ── timers ──
    uiTimer_.setInterval(kUiTickMs);
    uiTimer_.setTimerType(Qt::PreciseTimer);
    connect(&uiTimer_, &QTimer::timeout, this, &MainWindow::onUiTick);

    portWatchTimer_.setInterval(2000);
    connect(&portWatchTimer_, &QTimer::timeout, this, &MainWindow::watchPorts);
    portWatchTimer_.start();

    // Timebase changes are debounced: one K command after the user stops scrolling
    kDebounce_.setSingleShot(true);
    kDebounce_.setInterval(400);
    connect(&kDebounce_, &QTimer::timeout, this, [this] {
        const auto& tb = rfpm::timebases()[timebaseIndex_];
        if (link_.isOpen() && tb.k != link_.sentK()) {
            link_.setSampleRate(tb.k);
            scope_->clearSweep();
            showMeterStatus(QStringLiteral("sample rate K%1 sent: the stream pauses ~1 s").arg(tb.k, 2, 10, QLatin1Char('0')),
                            theme::TextDim);
        }
    });

    loadSettings();
    if (!options_.port.isEmpty()) portCombo_->setProperty("wanted", options_.port);
    refreshPorts(true);
    installShortcuts();

    resetStats();
    updateReadoutIdle();
    updateConnectionUi();
    updateMarkerReadouts();
    updateSweepBadge();
    chart_->setData(&chartBins_, 0, 60, true, peakHold_, floorDbm_);

    if (options_.size.isValid()) {
        resize(options_.size);
        move(10, 10);
    }
}

MainWindow::~MainWindow()
{
    // The link outlives nothing here: make sure it can't call back into a half-destroyed window
    link_.disconnect(this);
    link_.close();
    stopCsvLog();
}

void MainWindow::showEvent(QShowEvent* e)
{
    QMainWindow::showEvent(e);
    static bool first = true;
    if (!first) return;
    first = false;
    theme::applyNativeTitleBar(this);

    QTimer::singleShot(0, this, [this] {
        if (options_.simulate) {
            connectSimulator();
        } else if (options_.connect || actAutoConnect_->isChecked()) {
            if (!selectedPort().isEmpty()) connectSelected();
        }
    });

    if (options_.startTab >= 0) tabs_->setCurrentIndex(std::clamp(options_.startTab, 0, 2));
    if (!options_.screenshotPath.isEmpty()) {
        QTimer::singleShot(5000, this, [this] {
            grab().save(options_.screenshotPath);
            close();
        });
    }
}

void MainWindow::resizeEvent(QResizeEvent* e)
{
    QMainWindow::resizeEvent(e);
    brandTag_->setVisible(width() >= 1150);   // leave the room to the live status
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    uiTimer_.stop();
    saveSettings();
    stopCsvLog();
    link_.close();
    QMainWindow::closeEvent(e);
}

// ─── connection bar ─────────────────────────────────────────────────────────────

QWidget* MainWindow::buildConnectionBar()
{
    auto* bar = new QFrame;
    theme::makeCard(bar, "bar");
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(14, 8, 10, 8);
    h->setSpacing(8);

    // Brand: wave glyph + name
    const qreal dpr = devicePixelRatioF();
    QPixmap logo(QSize(26, 16) * dpr);
    logo.setDevicePixelRatio(dpr);
    logo.fill(Qt::transparent);
    {
        QPainter p(&logo);
        theme::paintLogo(p, QRectF(2, 2, 22, 12));
    }
    auto* logoLabel = new QLabel;
    logoLabel->setPixmap(logo);
    h->addWidget(logoLabel);
    auto* brand = new QLabel(QStringLiteral("<span style='font-weight:600'>RF POWER METER</span>"
                                            "<span style='color:%1; font-weight:600'> V5</span>")
                                 .arg(css(theme::AccentHot)));
    h->addWidget(brand);
    brandTag_ = makeLabel(QStringLiteral("assistant"), "faint");   // hidden on narrow windows
    h->addWidget(brandTag_);
    h->addSpacing(18);

    h->addWidget(caption(QStringLiteral("Port")));
    portCombo_ = new QComboBox;
    portCombo_->setMinimumWidth(200);
    portCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    portCombo_->setToolTip(QStringLiteral("Serial port. The meter (USB 0483:5740) is picked automatically."));
    h->addWidget(portCombo_);
    rescanButton_ = makeButton(QStringLiteral("Rescan"), QStringLiteral("Look for serial ports again (F5)"));
    connect(rescanButton_, &QPushButton::clicked, this, [this] { refreshPorts(false); });
    h->addWidget(rescanButton_);

    h->addSpacing(6);
    connectButton_ = makeButton(QStringLiteral("Connect"), QStringLiteral("Open / close the port (Ctrl+K)"));
    connectButton_->setProperty("accent", true);
    connectButton_->setMinimumWidth(116);
    connect(connectButton_, &QPushButton::clicked, this, &MainWindow::toggleConnection);
    h->addWidget(connectButton_);

    h->addSpacing(12);
    statusDot_ = new StatusDot;
    h->addWidget(statusDot_);
    statusText_ = new QLabel(QStringLiteral("disconnected"));
    statusText_->setFont(theme::monoFont(9));
    statusText_->setProperty("role", "dim");
    statusText_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusText_->setMinimumWidth(150);
    h->addWidget(statusText_, 1);

    optionsButton_ = new QToolButton;
    optionsButton_->setText(QStringLiteral("Options  ▾"));
    optionsButton_->setPopupMode(QToolButton::InstantPopup);
    optionsButton_->setCursor(Qt::PointingHandCursor);
    auto* menu = new QMenu(optionsButton_);
    baudMenu_ = menu->addMenu(QStringLiteral("Baud rate"));
    baudMenu_->setToolTipsVisible(true);
    baudGroup_ = new QActionGroup(this);
    for (int b : {460800, 921600, 230400, 115200}) {
        QAction* a = baudMenu_->addAction(QString::number(b));
        a->setCheckable(true);
        a->setData(b);
        a->setChecked(b == rfpm::kDefaultBaud);
        a->setToolTip(QStringLiteral("460800 for the USB-UART variant. The STM32 virtual COM port ignores it."));
        baudGroup_->addAction(a);
    }
    actDtr_ = menu->addAction(QStringLiteral("Assert DTR / RTS on connect"));
    actDtr_->setCheckable(true);
    actDtr_->setChecked(true);
    actDtr_->setToolTip(QStringLiteral("Some STM32 VCP firmwares only stream with DTR set"));
    actAutoConnect_ = menu->addAction(QStringLiteral("Connect automatically on start"));
    actAutoConnect_->setCheckable(true);
    actAutoReconnect_ = menu->addAction(QStringLiteral("Reconnect when the meter is plugged back in"));
    actAutoReconnect_->setCheckable(true);
    actAutoReconnect_->setChecked(true);
    menu->addSeparator();
    menu->addAction(QStringLiteral("Demo mode (simulated meter)"), this, &MainWindow::connectSimulator);
    menu->addSeparator();
    menu->addAction(QStringLiteral("Open the log folder"), this, &MainWindow::openLogFolder);
    menu->addAction(QStringLiteral("Keyboard shortcuts"), this, [this] {
        QMessageBox::information(this, QStringLiteral("Keyboard shortcuts"),
                                 QStringLiteral("<table cellpadding=3>"
                                                "<tr><td><b>Ctrl+K</b></td><td>Connect / disconnect</td></tr>"
                                                "<tr><td><b>F5</b></td><td>Rescan ports</td></tr>"
                                                "<tr><td><b>Ctrl+1 / 2 / 3</b></td><td>Monitor / Sweep / Log</td></tr>"
                                                "<tr><td><b>Ctrl+Space</b></td><td>Run / stop the sweep view</td></tr>"
                                                "<tr><td><b>Ctrl+T</b></td><td>Single sweep (arm the trigger)</td></tr>"
                                                "<tr><td><b>Ctrl+F</b></td><td>Auto-fit the sweep view</td></tr>"
                                                "<tr><td><b>Ctrl+E</b></td><td>Export the sweep to CSV</td></tr>"
                                                "<tr><td><b>Ctrl+S</b></td><td>Save the current chart as PNG</td></tr>"
                                                "<tr><td><b>Ctrl+L</b></td><td>Start / stop the CSV log</td></tr>"
                                                "</table>"));
    });
    menu->addAction(QStringLiteral("About"), this, [this] {
        QMessageBox::about(
            this, QStringLiteral("About"),
            QStringLiteral("<h3>RF Power Meter V5 — Assistant</h3>"
                           "<p>Version %1 · Qt %2</p>"
                           "<p>Companion for the USB RF Power Meter V5 (STM32 + AD8317/AD8318).</p>"
                           "<p>Max input ≈ +12 dBm: use an attenuator for anything stronger, and enter its "
                           "loss as the attenuation so the readings stay absolute.</p>")
                .arg(QApplication::applicationVersion(), QString::fromLatin1(qVersion())));
    });
    optionsButton_->setMenu(menu);
    h->addWidget(optionsButton_);

    connect(portCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        portCombo_->setToolTip(portCombo_->currentData(Qt::ToolTipRole).toString());
    });
    // activated = picked by the user (not by a rescan): remember it over automatic choices
    connect(portCombo_, QOverload<int>::of(&QComboBox::activated), this,
            [this] { portCombo_->setProperty("userChoice", selectedPort()); });
    return bar;
}

// ─── left column ────────────────────────────────────────────────────────────────

QWidget* MainWindow::buildLeftColumn()
{
    auto* panel = new QWidget;
    panel->setObjectName(QStringLiteral("leftPanel"));
    auto* v = new QVBoxLayout(panel);
    v->setContentsMargins(0, 0, 8, 0);
    v->setSpacing(12);
    v->addWidget(buildReadoutCard());
    v->addWidget(buildStatsCard());
    v->addWidget(buildMeterCard());
    v->addWidget(buildEventsCard());
    v->addWidget(buildCsvCard());
    v->addStretch(1);

    auto* scroll = new QScrollArea;
    scroll->setWidget(panel);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->viewport()->setAutoFillBackground(false);
    panel->setAutoFillBackground(false);
    scroll->setFixedWidth(372);
    return scroll;
}

QWidget* MainWindow::buildReadoutCard()
{
    QVBoxLayout* v = nullptr;
    QFrame* card = makeCard(&v, "hero");
    v->setSpacing(2);
    card->setToolTip(QStringLiteral("Mean power of the last 50 ms. Max safe input ≈ +12 dBm; "
                                    "linear range ≈ -50 … -5 dBm (AD8317)."));

    dbmReadout_ = new GlowReadout;
    v->addWidget(dbmReadout_);
    auto* unit = makeLabel(QStringLiteral("dBm"), "faint");
    unit->setAlignment(Qt::AlignCenter);
    unit->setFont(theme::uiFont(8, QFont::DemiBold));
    v->addWidget(unit);

    wattsLabel_ = new QLabel(QStringLiteral("—"));
    wattsLabel_->setAlignment(Qt::AlignCenter);
    wattsLabel_->setFont(theme::monoFont(15));
    setColor(wattsLabel_, theme::Watts);
    v->addWidget(wattsLabel_);
    v->addSpacing(10);

    signalBar_ = new SignalBar;
    signalBar_->setToolTip(QStringLiteral("Level, -80 … +10 dBm. The pink tick is the peak hold."));
    v->addWidget(signalBar_);
    v->addSpacing(8);

    overloadLabel_ = new QLabel;
    overloadLabel_->setWordWrap(true);
    setColor(overloadLabel_, theme::Bad);
    overloadLabel_->setFont(theme::uiFont(9, QFont::DemiBold));
    overloadLabel_->hide();
    v->addWidget(overloadLabel_);

    peakLabel_ = new QLabel(QStringLiteral("Peak hold: —"));
    peakLabel_->setWordWrap(true);
    setColor(peakLabel_, theme::Peak);
    v->addWidget(peakLabel_);
    floorLabel_ = new QLabel(QStringLiteral("Floor: —"));
    floorLabel_->setWordWrap(true);
    setColor(floorLabel_, theme::Floor);
    v->addWidget(floorLabel_);
    floorDeltaLabel_ = new QLabel;
    floorDeltaLabel_->setWordWrap(true);
    floorDeltaLabel_->setFont(theme::uiFont(8.5));
    setColor(floorDeltaLabel_, theme::Floor);
    v->addWidget(floorDeltaLabel_);
    v->addSpacing(8);

    auto* buttons = new QWidget;
    auto* flow = new FlowLayout(buttons, 6, 6);
    auto* peakReset = makeButton(QStringLiteral("Reset peak"));
    connect(peakReset, &QPushButton::clicked, this, &MainWindow::resetPeak);
    flow->addWidget(peakReset);

    floorCaptureButton_ = makeButton(QStringLiteral("Capture floor"),
                                     QStringLiteral("Records the noise floor with the signal source OFF.\n"
                                                    "Click: 1 s average. Press and hold: averages for as long as you hold."));
    // press / release instead of click: a long hold averages for the whole hold
    connect(floorCaptureButton_, &QPushButton::pressed, this, [this] {
        if (!link_.isOpen()) {
            floorDeltaLabel_->setText(QStringLiteral("connect first"));
            return;
        }
        beginFloorCapture(true);
    });
    connect(floorCaptureButton_, &QPushButton::released, this, [this] {
        if (!floorCapturing_) return;
        const double now = clock_.elapsed() / 1000.0;
        if (now - floorStartedAt_ >= 1.0) finalizeFloorCapture();
        else floorEndAt_ = floorStartedAt_ + 1.0;
    });
    flow->addWidget(floorCaptureButton_);

    floorClearButton_ = makeButton(QStringLiteral("Clear floor"));
    floorClearButton_->setEnabled(false);
    connect(floorClearButton_, &QPushButton::clicked, this, &MainWindow::clearFloor);
    flow->addWidget(floorClearButton_);
    v->addWidget(buttons);
    return card;
}

QWidget* MainWindow::buildStatsCard()
{
    QVBoxLayout* v = nullptr;
    QFrame* card = makeCard(&v);
    v->addWidget(sectionHeader(QStringLiteral("Statistics · since reset")));
    auto* h = new QHBoxLayout;
    auto* col = new QVBoxLayout;
    col->setSpacing(2);
    for (QLabel** l : {&statMin_, &statMax_, &statAvg_, &statCount_}) {
        *l = new QLabel;
        (*l)->setFont(theme::monoFont(9.5));
        col->addWidget(*l);
    }
    setColor(statCount_, theme::TextDim);
    h->addLayout(col, 1);
    auto* reset = makeButton(QStringLiteral("Reset"));
    connect(reset, &QPushButton::clicked, this, &MainWindow::resetStats);
    h->addWidget(reset, 0, Qt::AlignTop);
    v->addLayout(h);
    return card;
}

QWidget* MainWindow::buildMeterCard()
{
    QVBoxLayout* v = nullptr;
    QFrame* card = makeCard(&v);
    v->addWidget(sectionHeader(QStringLiteral("Meter settings · on device")));
    meterReports_ = new QLabel(QStringLiteral("Meter reports: not read yet"));
    meterReports_->setFont(theme::monoFont(9.5));
    meterReports_->setWordWrap(true);
    v->addWidget(meterReports_);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(8);
    grid->setContentsMargins(0, 4, 0, 0);

    grid->addWidget(makeLabel(QStringLiteral("Frequency"), "dim"), 0, 0);
    freqSpin_ = new QSpinBox;
    freqSpin_->setRange(rfpm::kMinFrequencyMhz, rfpm::kMaxFrequencyMhz);
    freqSpin_->setSuffix(QStringLiteral(" MHz"));
    freqSpin_->setValue(1000);
    freqSpin_->setAccelerated(true);
    freqSpin_->setToolTip(QStringLiteral("Measurement frequency: selects the meter's band calibration.\n"
                                         "It does not filter: the detector sees everything from ~1 MHz to 8 GHz."));
    grid->addWidget(freqSpin_, 0, 1);
    auto* presets = new QToolButton;
    presets->setText(QStringLiteral("Bands ▾"));
    presets->setPopupMode(QToolButton::InstantPopup);
    presets->setCursor(Qt::PointingHandCursor);
    presets->setToolTip(QStringLiteral("Common frequencies"));
    auto* presetMenu = new QMenu(presets);
    const struct { int mhz; const char* name; } bands[] = {
        {144, "2 m amateur"}, {433, "433 MHz ISM"}, {868, "868 MHz ISM / LoRa EU"}, {915, "915 MHz ISM / LoRa US"},
        {1090, "ADS-B"}, {1296, "23 cm amateur"}, {1575, "GPS L1"}, {2400, "2.4 GHz band start"},
        {2450, "Wi-Fi / Bluetooth / microwave"}, {3500, "5G n78"}, {5200, "Wi-Fi 5 GHz low"},
        {5800, "5.8 GHz ISM / FPV"}, {5880, "5.8 GHz FPV high"},
    };
    for (const auto& b : bands) {
        const int mhz = b.mhz;
        presetMenu->addAction(QStringLiteral("%1 MHz  ·  %2").arg(mhz).arg(QString::fromUtf8(b.name)), this,
                              [this, mhz] { freqSpin_->setValue(mhz); });
    }
    presets->setMenu(presetMenu);
    grid->addWidget(presets, 0, 2);

    grid->addWidget(makeLabel(QStringLiteral("Attenuation"), "dim"), 1, 0);
    offsetSpin_ = new QDoubleSpinBox;
    offsetSpin_->setRange(-rfpm::kMaxOffsetDb, rfpm::kMaxOffsetDb);
    offsetSpin_->setDecimals(1);
    offsetSpin_->setSingleStep(0.5);
    offsetSpin_->setSuffix(QStringLiteral(" dB"));
    offsetSpin_->setToolTip(QStringLiteral("Loss of the attenuator / coupler in front of the meter (e.g. 30 for a 30 dB pad).\n"
                                           "The meter adds it to every reading, so the display shows the power at the source."));
    grid->addWidget(offsetSpin_, 1, 1);

    grid->addWidget(makeLabel(QStringLiteral("Timebase"), "dim"), 2, 0);
    timebaseCombo_ = new QComboBox;
    for (const auto& tb : rfpm::timebases()) {
        timebaseCombo_->addItem(QStringLiteral("%1   (K%2)").arg(qs(tb.label)).arg(tb.k, 2, 10, QLatin1Char('0')));
    }
    timebaseCombo_->setToolTip(QStringLiteral("Sweep length and sample rate (K01..K18). Fast rates stream ~3000 records/s;\n"
                                              "slow ones give long sweeps but update the readout less often."));
    connect(timebaseCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int i) { setTimebaseIndex(i, true); });
    grid->addWidget(timebaseCombo_, 2, 1, 1, 2);
    grid->setColumnStretch(1, 1);
    v->addLayout(grid);

    auto* row = new QHBoxLayout;
    row->setSpacing(6);
    applyButton_ = makeButton(QStringLiteral("Apply to meter"), QStringLiteral("Write frequency and attenuation, then read them back"));
    connect(applyButton_, &QPushButton::clicked, this, &MainWindow::applyToMeter);
    row->addWidget(applyButton_);
    readButton_ = makeButton(QStringLiteral("Read back"), QStringLiteral("Ask the meter for its current frequency and attenuation"));
    connect(readButton_, &QPushButton::clicked, this, [this] { link_.requestSettings(); });
    row->addWidget(readButton_);
    row->addStretch(1);
    v->addLayout(row);

    restoreCheck_ = new QCheckBox(QStringLiteral("Re-apply on connect"));
    restoreCheck_->setChecked(true);
    restoreCheck_->setToolTip(QStringLiteral("The meter forgets its settings when unplugged (back to 1 MHz, +0.0 dB).\n"
                                             "When checked, the last applied frequency and attenuation are restored on connect."));
    v->addWidget(restoreCheck_);

    meterStatus_ = new QLabel;
    meterStatus_->setWordWrap(true);
    meterStatus_->setFont(theme::uiFont(8.5));
    v->addWidget(meterStatus_);
    return card;
}

QWidget* MainWindow::buildEventsCard()
{
    QVBoxLayout* v = nullptr;
    QFrame* card = makeCard(&v);
    v->addWidget(sectionHeader(QStringLiteral("Threshold events")));
    auto* row = new QHBoxLayout;
    row->setSpacing(8);
    eventsEnable_ = new QCheckBox(QStringLiteral("Enable"));
    eventsEnable_->setToolTip(QStringLiteral("Logs a timestamp each time the level rises above the threshold.\n"
                                             "Re-arms once it falls 2 dB below."));
    row->addWidget(eventsEnable_);
    row->addStretch(1);
    row->addWidget(makeLabel(QStringLiteral("Threshold"), "dim"));
    eventsThreshold_ = new QDoubleSpinBox;
    eventsThreshold_->setRange(-99.0, 30.0);
    eventsThreshold_->setDecimals(1);
    eventsThreshold_->setSingleStep(1.0);
    eventsThreshold_->setValue(-40.0);
    eventsThreshold_->setSuffix(QStringLiteral(" dBm"));
    row->addWidget(eventsThreshold_);
    v->addLayout(row);

    eventsList_ = new QListWidget;
    eventsList_->setFixedHeight(92);
    eventsList_->setFont(theme::monoFont(9));
    v->addWidget(eventsList_);

    auto* bottom = new QHBoxLayout;
    eventsStatus_ = makeLabel(QStringLiteral("armed when enabled"), "faint");
    eventsStatus_->setFont(theme::uiFont(8.5));
    bottom->addWidget(eventsStatus_, 1);
    auto* copy = makeButton(QStringLiteral("Copy"), QStringLiteral("Copy the events to the clipboard"));
    connect(copy, &QPushButton::clicked, this, [this] {
        QStringList lines;
        for (int i = eventsList_->count() - 1; i >= 0; --i) lines << eventsList_->item(i)->text();
        QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    });
    bottom->addWidget(copy);
    auto* clear = makeButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, eventsList_, &QListWidget::clear);
    bottom->addWidget(clear);
    v->addLayout(bottom);

    connect(eventsEnable_, &QCheckBox::toggled, this, [this](bool on) {
        eventArmed_ = true;
        eventBinsAbove_ = 0;
        eventsStatus_->setText(on ? QStringLiteral("armed") : QStringLiteral("armed when enabled"));
    });
    return card;
}

QWidget* MainWindow::buildCsvCard()
{
    QVBoxLayout* v = nullptr;
    QFrame* card = makeCard(&v);
    v->addWidget(sectionHeader(QStringLiteral("CSV log")));
    auto* h = new QHBoxLayout;
    csvStatus_ = makeLabel(QStringLiteral("not logging — 20 rows/s: time, avg, min, max"), "faint");
    csvStatus_->setWordWrap(true);
    csvStatus_->setFont(theme::uiFont(8.5));
    h->addWidget(csvStatus_, 1);
    auto* folder = makeButton(QStringLiteral("Folder"), QStringLiteral("Open Documents/rfmeter/logs"));
    connect(folder, &QPushButton::clicked, this, &MainWindow::openLogFolder);
    h->addWidget(folder);
    csvButton_ = makeButton(QStringLiteral("Start"), QStringLiteral("Start / stop logging (Ctrl+L)"));
    csvButton_->setMinimumWidth(72);
    connect(csvButton_, &QPushButton::clicked, this, &MainWindow::toggleCsvLog);
    h->addWidget(csvButton_);
    v->addLayout(h);
    return card;
}

// ─── tabs ───────────────────────────────────────────────────────────────────────

QWidget* MainWindow::buildMonitorTab()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 4, 0, 0);
    v->setSpacing(8);

    auto* toolbar = new QWidget;
    auto* flow = new FlowLayout(toolbar, 8, 6);
    windowCombo_ = new QComboBox;
    for (const char* l : kChartWindowLabels) windowCombo_->addItem(QString::fromLatin1(l));
    windowCombo_->setCurrentIndex(2);
    flow->addWidget(group({caption(QStringLiteral("Window")), windowCombo_}));
    flow->addSpacing(8);
    maxTraceCheck_ = new QCheckBox(QStringLiteral("Max trace"));
    maxTraceCheck_->setChecked(true);
    maxTraceCheck_->setToolTip(QStringLiteral("Thin line: the highest sample of each point (catches short pulses)"));
    flow->addWidget(maxTraceCheck_);
    flow->addSpacing(12);
    auto* legend = new QLabel(QStringLiteral("<span style='color:%1'>trace color = strength:</span> "
                                             "<span style='color:#3B82F6'>● low</span> "
                                             "<span style='color:#F59E0B'>● mid</span> "
                                             "<span style='color:#EF4444'>● hot</span> "
                                             "<span style='color:%2'>&nbsp;┄ peak hold</span> "
                                             "<span style='color:%3'>&nbsp;┈ floor</span>")
                                  .arg(css(theme::TextFaint), css(theme::TextDim), css(theme::Floor)));
    legend->setFont(theme::uiFont(8.5));
    flow->addWidget(legend);
    flow->addSpacing(12);
    auto* clear = makeButton(QStringLiteral("Clear history"));
    connect(clear, &QPushButton::clicked, this, [this] {
        chartBins_.clear();
        chart_->update();
    });
    flow->addWidget(clear);
    auto* save = makeButton(QStringLiteral("Save image"), QStringLiteral("Save the chart as PNG (Ctrl+S)"));
    connect(save, &QPushButton::clicked, this, &MainWindow::saveImage);
    flow->addWidget(save);
    v->addWidget(toolbar);

    chart_ = new TimeSeriesChart;
    v->addWidget(chart_, 1);
    return page;
}

QWidget* MainWindow::buildSweepTab()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 4, 0, 0);
    v->setSpacing(8);

    auto* toolbar = new QWidget;
    auto* flow = new FlowLayout(toolbar, 8, 6);
    runButton_ = makeButton(QStringLiteral("Stop"), QStringLiteral("Freeze / resume the trace (Ctrl+Space). The readouts stay live."));
    runButton_->setMinimumWidth(70);
    connect(runButton_, &QPushButton::clicked, this, [this] { setSweepRunning(!sweepRunning_); });
    singleButton_ = makeButton(QStringLiteral("Single"), QStringLiteral("Capture the next triggered sweep, then stop (Ctrl+T)"));
    connect(singleButton_, &QPushButton::clicked, this, &MainWindow::armSingle);

    sweepTimebaseCombo_ = new QComboBox;
    for (const auto& tb : rfpm::timebases()) sweepTimebaseCombo_->addItem(qs(tb.label));
    sweepTimebaseCombo_->setToolTip(timebaseCombo_->toolTip());
    connect(sweepTimebaseCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int i) { setTimebaseIndex(i, true); });

    trigModeCombo_ = new QComboBox;
    trigModeCombo_->addItems({QStringLiteral("Auto"), QStringLiteral("Normal"), QStringLiteral("Single")});
    trigModeCombo_->setToolTip(QStringLiteral("Auto: show every sweep\nNormal: only sweeps that cross the trigger level\n"
                                              "Single: the next crossing sweep, then stop"));
    connect(trigModeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int mode) {
        scope_->setTriggerArmed(mode != 0);
        waitingForTrigger_ = false;
        if (mode == 2) setSweepRunning(true);
        updateSweepBadge();
    });
    trigEdgeCombo_ = new QComboBox;
    trigEdgeCombo_->addItems({QStringLiteral("↗ Rising"), QStringLiteral("↘ Falling")});
    trigEdgeCombo_->setToolTip(QStringLiteral("Trigger edge"));
    trigLevelSpin_ = new QDoubleSpinBox;
    trigLevelSpin_->setRange(-99.0, 30.0);
    trigLevelSpin_->setDecimals(1);
    trigLevelSpin_->setSingleStep(0.5);
    trigLevelSpin_->setSuffix(QStringLiteral(" dBm"));
    trigLevelSpin_->setToolTip(QStringLiteral("Trigger level (or drag the T handle on the right of the scope)"));

    scaleCombo_ = new QComboBox;
    for (double s : SweepScope::scales()) scaleCombo_->addItem(QStringLiteral("%1 dB/div").arg(s));
    scaleCombo_->setToolTip(QStringLiteral("Vertical scale (Ctrl+wheel on the scope)"));
    topSpin_ = new QDoubleSpinBox;
    topSpin_->setRange(-100, 40);
    topSpin_->setDecimals(1);
    topSpin_->setSuffix(QStringLiteral(" dBm"));
    topSpin_->setToolTip(QStringLiteral("Level at the top of the screen (wheel on the scope)"));
    auto* fit = makeButton(QStringLiteral("Auto-fit"), QStringLiteral("Fit the view to the trace (Ctrl+F, or double-click the scope)"));
    auto* clear = makeButton(QStringLiteral("Clear"), QStringLiteral("Clear the trace"));
    auto* exportButton = makeButton(QStringLiteral("Export CSV"), QStringLiteral("Save the displayed sweep: Index, Time_s, dBm, Watt (Ctrl+E)"));
    auto* save = makeButton(QStringLiteral("Save image"), QStringLiteral("Save the scope as PNG (Ctrl+S)"));

    flow->addWidget(group({runButton_, singleButton_}));
    flow->addSpacing(6);
    flow->addWidget(group({caption(QStringLiteral("Timebase")), sweepTimebaseCombo_}));
    flow->addSpacing(6);
    flow->addWidget(group({caption(QStringLiteral("Trigger")), trigModeCombo_, trigEdgeCombo_, trigLevelSpin_}));
    flow->addSpacing(6);
    flow->addWidget(group({caption(QStringLiteral("Scale")), scaleCombo_}));
    flow->addWidget(group({caption(QStringLiteral("Top")), topSpin_, fit}));
    flow->addSpacing(6);
    flow->addWidget(group({clear, exportButton, save}));
    v->addWidget(toolbar);

    // Marker readouts
    auto* strip = new QFrame;
    theme::makeCard(strip, "strip");
    auto* sh = new FlowLayout(strip, 22, 4);
    sh->setContentsMargins(12, 6, 12, 6);
    m1Label_ = new QLabel;
    m2Label_ = new QLabel;
    deltaLabel_ = new QLabel;
    sweepStatsLabel_ = new QLabel;
    for (QLabel* l : {m1Label_, m2Label_, deltaLabel_, sweepStatsLabel_}) {
        l->setFont(theme::monoFont(9));
        l->setTextFormat(Qt::RichText);
    }
    sh->addWidget(m1Label_);
    sh->addWidget(m2Label_);
    sh->addWidget(deltaLabel_);
    sh->addWidget(sweepStatsLabel_);
    v->addWidget(strip);

    scope_ = new SweepScope;
    v->addWidget(scope_, 1);

    auto* hint = makeLabel(QStringLiteral("Click or drag on the scope to move the markers · drag the T handle to set the trigger level · "
                                          "wheel moves the view, Ctrl+wheel zooms · double-click auto-fits"),
                           "hint");
    hint->setFont(theme::uiFont(8));
    hint->setWordWrap(true);
    v->addWidget(hint);

    // ── wiring ──
    connect(trigLevelSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), scope_, &SweepScope::setTriggerLevel);
    connect(scope_, &SweepScope::triggerLevelChanged, this, [this](double level) {
        const QSignalBlocker block(trigLevelSpin_);
        trigLevelSpin_->setValue(level);
    });
    connect(scaleCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
        if (i < 0) return;
        scope_->setView(SweepScope::scales()[i], topSpin_->value());
        topSpin_->setSingleStep(SweepScope::scales()[i]);
    });
    connect(topSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double top) { scope_->setView(scope_->scale(), top); });
    connect(scope_, &SweepScope::viewChanged, this, &MainWindow::syncViewControls);
    connect(scope_, &SweepScope::markersChanged, this, &MainWindow::updateMarkerReadouts);
    connect(fit, &QPushButton::clicked, scope_, &SweepScope::autoFit);
    connect(clear, &QPushButton::clicked, this, [this] {
        scope_->clearSweep();
        sweepCount_ = 0;
        updateMarkerReadouts();
    });
    connect(exportButton, &QPushButton::clicked, this, &MainWindow::exportSweep);
    connect(save, &QPushButton::clicked, this, &MainWindow::saveImage);
    return page;
}

QWidget* MainWindow::buildLogTab()
{
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 4, 0, 0);
    v->setSpacing(8);

    logView_ = new QPlainTextEdit;
    logView_->setReadOnly(true);
    logView_->setFont(theme::monoFont(9.5));
    logView_->setMaximumBlockCount(3000);
    logView_->setPlaceholderText(QStringLiteral("S- = sent, R- = received, E- = error"));
    v->addWidget(logView_, 1);

    auto* row = new QHBoxLayout;
    row->addWidget(caption(QStringLiteral("Waveform data")));
    rawRecordLabel_ = new QLabel(QStringLiteral("—"));
    rawRecordLabel_->setFont(theme::monoFont(10));
    rawRecordLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rawRecordLabel_->setToolTip(QStringLiteral("The last raw record, e.g. -63300000u = -63.3 dBm, 000.00 µW"));
    row->addWidget(rawRecordLabel_);
    row->addStretch(1);
    auto* copy = makeButton(QStringLiteral("Copy"));
    connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(logView_->toPlainText()); });
    row->addWidget(copy);
    auto* clear = makeButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, logView_, &QPlainTextEdit::clear);
    row->addWidget(clear);
    v->addLayout(row);
    return page;
}

void MainWindow::installShortcuts()
{
    auto add = [this](const QKeySequence& seq, auto fn) {
        auto* sc = new QShortcut(seq, this);
        connect(sc, &QShortcut::activated, this, fn);
    };
    add(QKeySequence(Qt::CTRL | Qt::Key_K), [this] { toggleConnection(); });
    add(QKeySequence(Qt::Key_F5), [this] { if (!link_.isOpen()) refreshPorts(false); });
    add(QKeySequence(Qt::CTRL | Qt::Key_1), [this] { tabs_->setCurrentIndex(0); });
    add(QKeySequence(Qt::CTRL | Qt::Key_2), [this] { tabs_->setCurrentIndex(1); });
    add(QKeySequence(Qt::CTRL | Qt::Key_3), [this] { tabs_->setCurrentIndex(2); });
    add(QKeySequence(Qt::CTRL | Qt::Key_Space), [this] { setSweepRunning(!sweepRunning_); });
    add(QKeySequence(Qt::CTRL | Qt::Key_T), [this] { armSingle(); });
    add(QKeySequence(Qt::CTRL | Qt::Key_F), [this] { scope_->autoFit(); });
    add(QKeySequence(Qt::CTRL | Qt::Key_E), [this] { exportSweep(); });
    add(QKeySequence(Qt::CTRL | Qt::Key_S), [this] { saveImage(); });
    add(QKeySequence(Qt::CTRL | Qt::Key_L), [this] { toggleCsvLog(); });
}

// ═════════════════════════════════════════════════════════════════════════════════
//  connection
// ═════════════════════════════════════════════════════════════════════════════════

QString MainWindow::selectedPort() const
{
    return portCombo_->currentData().toString();
}

int MainWindow::selectedBaud() const
{
    const QAction* a = baudGroup_->checkedAction();
    return a ? a->data().toInt() : rfpm::kDefaultBaud;
}

void MainWindow::refreshPorts(bool quiet)
{
    // Selection priority: --port, the user's pick this session, the last used port, the meter
    // (by USB id), whatever was selected, the first port.
    const QString wanted = portCombo_->property("wanted").toString();
    portCombo_->setProperty("wanted", QVariant());
    const QString userChoice = portCombo_->property("userChoice").toString();
    const QString current = selectedPort();

    auto infos = QSerialPortInfo::availablePorts();
    std::sort(infos.begin(), infos.end(), [](const QSerialPortInfo& a, const QSerialPortInfo& b) {
        const QString an = a.portName(), bn = b.portName();
        return an.size() != bn.size() ? an.size() < bn.size() : an < bn;   // COM2 before COM10
    });

    const QSignalBlocker block(portCombo_);
    portCombo_->clear();
    knownPorts_.clear();
    QString meterPort;
    for (const QSerialPortInfo& info : infos) {
        if (!listedPort(info)) continue;
        const bool isMeter = info.hasVendorIdentifier() && info.hasProductIdentifier()
                             && info.vendorIdentifier() == rfpm::kUsbVendorId && info.productIdentifier() == rfpm::kUsbProductId;
        QString text = info.portName();
        if (isMeter) {
            text += QStringLiteral("  ·  RF meter");
            if (meterPort.isEmpty()) meterPort = info.portName();
        } else if (!info.description().isEmpty()) {
            QString d = info.description();
            if (d.size() > 26) d = d.left(25) + QStringLiteral("…");
            text += QStringLiteral("  ·  ") + d;
        }
        QString tip = info.description().isEmpty() ? info.portName() : info.description();
        tip += QStringLiteral("\n") + info.systemLocation();
        if (info.hasVendorIdentifier()) {
            tip += QStringLiteral("\nUSB %1:%2")
                       .arg(info.vendorIdentifier(), 4, 16, QLatin1Char('0'))
                       .arg(info.productIdentifier(), 4, 16, QLatin1Char('0'));
        }
        portCombo_->addItem(text, info.portName());
        portCombo_->setItemData(portCombo_->count() - 1, tip, Qt::ToolTipRole);
        knownPorts_ << info.portName();
    }

    int index = -1;
    const QString saved = QSettings().value(QStringLiteral("connection/port")).toString();
    for (const QString& want : {wanted, userChoice, saved, meterPort, current}) {
        if (want.isEmpty()) continue;
        index = portCombo_->findData(want);
        if (index >= 0) break;
    }
    if (index < 0 && portCombo_->count() > 0) index = 0;
    portCombo_->setCurrentIndex(index);
    portCombo_->setToolTip(portCombo_->currentData(Qt::ToolTipRole).toString());

    if (!quiet) {
        appendLog(MeterLink::LogInfo, knownPorts_.isEmpty() ? QStringLiteral("No serial ports found")
                                                            : QStringLiteral("Ports: %1").arg(knownPorts_.join(QStringLiteral(", "))));
        if (!link_.isOpen() && knownPorts_.isEmpty()) {
            statusText_->setText(QStringLiteral("no serial ports — plug the meter in, or try Options ▸ Demo mode"));
        }
    }
}

void MainWindow::watchPorts()
{
    QStringList names;
    for (const QSerialPortInfo& info : QSerialPortInfo::availablePorts()) {
        if (listedPort(info)) names << info.portName();
    }

    if (link_.isOpen()) {
        // Some drivers never report an error when the device is unplugged: notice it here
        if (!link_.isSimulated() && !names.contains(link_.portName())) link_.close(QStringLiteral("device removed"));
        return;
    }
    QStringList known = knownPorts_;
    std::sort(names.begin(), names.end());
    std::sort(known.begin(), known.end());
    if (names != known) refreshPorts(true);

    if (!reconnectPort_.isEmpty() && actAutoReconnect_->isChecked() && names.contains(reconnectPort_)) {
        const int i = portCombo_->findData(reconnectPort_);
        if (i >= 0) {
            portCombo_->setCurrentIndex(i);
            appendLog(MeterLink::LogInfo, QStringLiteral("%1 is back: reconnecting").arg(reconnectPort_));
            connectSelected();
        }
    }
}

void MainWindow::toggleConnection()
{
    reconnectPort_.clear();
    if (link_.isOpen()) {
        link_.close();
        return;
    }
    connectSelected();
}

void MainWindow::connectSelected()
{
    const QString port = selectedPort();
    if (port.isEmpty()) {
        statusText_->setText(QStringLiteral("no port selected"));
        return;
    }
    QString error;
    if (!link_.openSerial(port, selectedBaud(), actDtr_->isChecked(), &error)) {
        statusDot_->setColor(theme::Bad);
        statusText_->setText(QStringLiteral("open %1 failed: %2").arg(port, error));
        appendLog(MeterLink::LogError, QStringLiteral("open %1 failed: %2").arg(port, error));
    }
}

void MainWindow::connectSimulator()
{
    reconnectPort_.clear();
    link_.openSimulator();
}

void MainWindow::onConnectionChanged(bool open, const QString& reason)
{
    const double now = clock_.elapsed() / 1000.0;
    if (open) {
        lastTotalRecords_ = 0;
        lastRateAt_ = lastDataAt_ = now;
        currentRate_ = 0;
        meterFrequency_.reset();
        boxesPrefilled_ = false;
        pendingSettings_.reset();
        sweepCount_ = 0;
        scope_->clearSweep();
        uiTimer_.start();

        // The sample rate is write-only on the meter: set it so we know what it is.
        link_.setSampleRate(rfpm::timebases()[timebaseIndex_].k, 150);

        // The meter forgets everything on power-cycle: restore the last applied band
        QSettings s;
        const QVariant freq = s.value(QStringLiteral("meter/appliedFrequency"));
        const QVariant offset = s.value(QStringLiteral("meter/appliedOffset"));
        if (restoreCheck_->isChecked() && freq.isValid() && offset.isValid()) {
            QString error;
            if (link_.applySettings(freq.toInt(), offset.toDouble(), &error)) {
                pendingSettings_ = PendingSettings{freq.toInt(), offset.toDouble()};
                showMeterStatus(QStringLiteral("restoring %1 MHz, %2 dB…").arg(freq.toInt()).arg(qs(rfpm::formatOffset(offset.toDouble()))),
                                theme::TextDim);
            }
        } else {
            link_.requestSettings(1200);
            showMeterStatus(QString(), theme::TextDim);
        }
    } else {
        uiTimer_.stop();
        stopCsvLog();
        if (floorCapturing_) finalizeFloorCapture();
        pendingSettings_.reset();
        if (!reason.isEmpty() && !link_.isSimulated() && link_.portName() != QLatin1String("Simulator")) {
            reconnectPort_ = link_.portName();
        }
        updateReadoutIdle();
    }
    updateConnectionUi();
    if (!open && !reason.isEmpty()) {
        statusDot_->setColor(theme::Bad);
        statusText_->setText(actAutoReconnect_->isChecked() && !reconnectPort_.isEmpty()
                                 ? QStringLiteral("%1 lost (%2) — waiting for it to come back…").arg(reconnectPort_, reason)
                                 : QStringLiteral("port error: %1").arg(reason));
    }
}

void MainWindow::updateConnectionUi()
{
    const bool open = link_.isOpen();
    connectButton_->setText(open ? QStringLiteral("Disconnect") : QStringLiteral("Connect"));
    portCombo_->setEnabled(!open);
    baudMenu_->setEnabled(!open);
    rescanButton_->setEnabled(!open);
    applyButton_->setEnabled(open);
    readButton_->setEnabled(open);
    statusDot_->setColor(open ? theme::AccentHot : theme::Idle);
    if (open) {
        statusText_->setText(link_.isSimulated() ? QStringLiteral("demo mode — simulated meter")
                                                 : QStringLiteral("%1 @ %2").arg(link_.portName()).arg(link_.baudRate()));
    } else {
        statusText_->setText(QStringLiteral("disconnected"));
    }
    const QString empty = open ? QStringLiteral("Waiting for data…")
                               : QStringLiteral("Connect the meter to start streaming  (or Options ▸ Demo mode)");
    chart_->setEmptyMessage(empty);
    scope_->setEmptyMessage(open ? QStringLiteral("Waiting for a sweep…") : empty);
    updateSweepBadge();
}

// ═════════════════════════════════════════════════════════════════════════════════
//  meter settings
// ═════════════════════════════════════════════════════════════════════════════════

void MainWindow::showMeterStatus(const QString& text, const QColor& color)
{
    meterStatus_->setText(text);
    setColor(meterStatus_, color);
    meterStatus_->setVisible(!text.isEmpty());
}

void MainWindow::applyToMeter()
{
    if (!link_.isOpen()) {
        showMeterStatus(QStringLiteral("connect first"), theme::Bad);
        return;
    }
    const int freq = freqSpin_->value();
    const double offset = std::round(offsetSpin_->value() * 10.0) / 10.0;
    QString error;
    if (!link_.applySettings(freq, offset, &error)) {
        showMeterStatus(error, theme::Bad);
        return;
    }
    pendingSettings_ = PendingSettings{freq, offset};
    showMeterStatus(QStringLiteral("sent, verifying…"), theme::TextDim);
}

void MainWindow::onSettingsReceived(int frequencyMhz, double offsetDb)
{
    meterFrequency_ = frequencyMhz;
    meterReports_->setText(QStringLiteral("Meter reports: %1 MHz, %2 dB").arg(frequencyMhz).arg(qs(rfpm::formatOffset(offsetDb))));

    if (!boxesPrefilled_ && !pendingSettings_) {
        freqSpin_->setValue(frequencyMhz);
        offsetSpin_->setValue(offsetDb);
    }
    boxesPrefilled_ = true;

    if (pendingSettings_) {
        const PendingSettings sent = *pendingSettings_;
        pendingSettings_.reset();
        const bool ok = frequencyMhz == sent.freq && std::fabs(offsetDb - sent.offset) < 0.05;
        if (ok) {
            QSettings s;
            s.setValue(QStringLiteral("meter/appliedFrequency"), sent.freq);
            s.setValue(QStringLiteral("meter/appliedOffset"), sent.offset);
            showMeterStatus(QStringLiteral("✔ applied: %1 MHz, %2 dB").arg(frequencyMhz).arg(qs(rfpm::formatOffset(offsetDb))),
                            theme::Good);
        } else {
            showMeterStatus(QStringLiteral("MISMATCH: sent %1 MHz %2, the meter reports %3 MHz %4")
                                .arg(sent.freq)
                                .arg(qs(rfpm::formatOffset(sent.offset)))
                                .arg(frequencyMhz)
                                .arg(qs(rfpm::formatOffset(offsetDb))),
                            theme::Bad);
        }
    } else if (frequencyMhz == 1) {
        showMeterStatus(QStringLiteral("The meter is at its power-on default (1 MHz): set the band you are measuring and Apply, "
                                       "or the readings can be several dB off."),
                        theme::Warn);
    }
}

void MainWindow::setTimebaseIndex(int index, bool sendToMeter)
{
    const int n = int(rfpm::timebases().size());
    index = std::clamp(index, 0, n - 1);
    timebaseIndex_ = index;
    {
        const QSignalBlocker b1(timebaseCombo_);
        const QSignalBlocker b2(sweepTimebaseCombo_);
        timebaseCombo_->setCurrentIndex(index);
        sweepTimebaseCombo_->setCurrentIndex(index);
    }
    const auto& tb = rfpm::timebases()[index];
    scope_->setTimebase(tb.windowSec, tb.samples, tb.periodSec);
    updateMarkerReadouts();
    updateSweepBadge();
    if (sendToMeter) kDebounce_.start();
}

// ═════════════════════════════════════════════════════════════════════════════════
//  live data
// ═════════════════════════════════════════════════════════════════════════════════

void MainWindow::updateReadoutIdle()
{
    dbmReadout_->setText(QStringLiteral("--.-"));
    wattsLabel_->setText(link_.isOpen() ? QStringLiteral("no data") : QStringLiteral("—"));
    signalBar_->setValue(std::numeric_limits<double>::quiet_NaN());
    overloadLabel_->hide();
}

void MainWindow::onUiTick()
{
    const double now = clock_.elapsed() / 1000.0;
    const SampleBin bin = link_.drainBin();

    if (bin.count > 0) {
        lastDataAt_ = now;
        const double avg = bin.avg();

        chartBins_.push_back({now, float(avg), float(bin.min), float(bin.max)});
        while (chartBins_.size() > kMaxChartBins) chartBins_.pop_front();

        dbmReadout_->setText(dbmText(avg));
        wattsLabel_->setText(wattsText(rfpm::dbmToWatts(avg)));
        signalBar_->setValue(avg);

        if (bin.max > peakHold_) {
            peakHold_ = bin.max;
            peakLabel_->setText(QStringLiteral("Peak hold: %1 dBm  (%2)").arg(dbmText(peakHold_), wattsText(rfpm::dbmToWatts(peakHold_))));
            signalBar_->setPeak(peakHold_);
        }

        statSum_ += bin.sum;
        statSamples_ += bin.count;
        statMinValue_ = std::min(statMinValue_, bin.min);
        statMaxValue_ = std::max(statMaxValue_, bin.max);
        statMin_->setText(QStringLiteral("Min:  %1 dBm").arg(dbmText(statMinValue_)));
        statMax_->setText(QStringLiteral("Max:  %1 dBm").arg(dbmText(statMaxValue_)));
        statAvg_->setText(QStringLiteral("Avg:  %1 dBm").arg(dbmText(statSum_ / statSamples_, 2)));
        statCount_->setText(QStringLiteral("      %L1 samples").arg(statSamples_));

        if (bin.max >= kOverloadDbm) {
            overloadLabel_->setText(QStringLiteral("⚠ %1 dBm: above the linear range, close to the +12 dBm damage level. "
                                                   "Add an attenuator.")
                                        .arg(dbmText(bin.max)));
            overloadLabel_->show();
        } else if (overloadLabel_->isVisible() && bin.max < kOverloadDbm - 3) {
            overloadLabel_->hide();
        }

        if (csvFile_.isOpen()) {
            csvStream_ << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) << ','
                       << QString::number(avg, 'f', 2) << ',' << QString::number(bin.min, 'f', 1) << ','
                       << QString::number(bin.max, 'f', 1) << ',' << bin.count << '\n';
            if (++csvRows_ % 20 == 0) {
                csvStream_.flush();
                csvStatus_->setText(QStringLiteral("logging %L1 rows → %2").arg(csvRows_).arg(QFileInfo(csvFile_.fileName()).fileName()));
            }
        }

        updateFloorReadout(bin, avg);
        processThresholdEvent(avg);
    } else if (link_.isOpen()) {
        // Slow timebases deliver a burst per sweep: only call it a stall after two sweeps
        const int k = link_.sentK() > 0 ? link_.sentK() : 1;
        const double limit = std::max(2.5, 2.0 * (rfpm::sweepDuration(k) + 0.3));
        if (now - lastDataAt_ > limit) updateReadoutIdle();
        if (floorCapturing_ && floorEndAt_ && now >= *floorEndAt_) finalizeFloorCapture();
    }

    // Once a second: the records/s figure and the raw record
    if (now - lastRateAt_ >= 1.0) {
        const qint64 total = link_.totalRecords();
        currentRate_ = (total - lastTotalRecords_) / (now - lastRateAt_);
        lastTotalRecords_ = total;
        lastRateAt_ = now;
        if (link_.isOpen()) {
            QString status = link_.isSimulated() ? QStringLiteral("demo") : link_.portName();
            status += QStringLiteral(" · %1 rec/s").arg(currentRate_, 0, 'f', 0);
            if (link_.sentK() > 0) status += QStringLiteral(" · K%1").arg(link_.sentK(), 2, 10, QLatin1Char('0'));
            const qint64 bad = link_.malformedTokens();
            if (bad > 1) status += QStringLiteral(" · %1 bad").arg(bad);
            if (now - lastDataAt_ > 3.0 && currentRate_ < 1) status += QStringLiteral(" · no data");
            statusText_->setText(status);
            statusText_->setToolTip(QStringLiteral("%1 @ %2 baud\n%3 records/s, %4 malformed tokens since connect\n"
                                                   "sample rate K%5 (write-only: confirmed by the record rate)")
                                        .arg(link_.isSimulated() ? QStringLiteral("simulated meter") : link_.portName())
                                        .arg(link_.baudRate())
                                        .arg(currentRate_, 0, 'f', 0)
                                        .arg(bad)
                                        .arg(link_.sentK(), 2, 10, QLatin1Char('0')));
        }
        const QString raw = link_.lastRecord();
        rawRecordLabel_->setText(raw.isEmpty() ? QStringLiteral("—") : raw);
    }

    const int w = windowCombo_->currentIndex();
    chart_->setData(&chartBins_, now, kChartWindows[std::clamp(w, 0, 4)], maxTraceCheck_->isChecked(), peakHold_, floorDbm_);
}

void MainWindow::resetPeak()
{
    peakHold_ = -std::numeric_limits<double>::infinity();
    peakLabel_->setText(QStringLiteral("Peak hold: —"));
    signalBar_->setPeak(std::numeric_limits<double>::quiet_NaN());
    chart_->update();
}

void MainWindow::resetStats()
{
    statMinValue_ = std::numeric_limits<double>::infinity();
    statMaxValue_ = -std::numeric_limits<double>::infinity();
    statSum_ = 0;
    statSamples_ = 0;
    statMin_->setText(QStringLiteral("Min:  —"));
    statMax_->setText(QStringLiteral("Max:  —"));
    statAvg_->setText(QStringLiteral("Avg:  —"));
    statCount_->setText(QStringLiteral("      0 samples"));
}

// ── noise-floor reference ("tare") ──────────────────────────────────────────────
// When captured, the readout shows the delta above the floor and the floor-corrected net
// level (measured watts minus floor watts): the honest RF contribution when a reading sits
// within a few dB of the floor.

void MainWindow::beginFloorCapture(bool openEnded)
{
    floorCapturing_ = true;
    floorStartedAt_ = clock_.elapsed() / 1000.0;
    floorEndAt_.reset();
    if (!openEnded) floorEndAt_ = floorStartedAt_ + 1.0;
    floorSum_ = 0;
    floorCount_ = 0;
    floorLabel_->setText(QStringLiteral("Floor: capturing…"));
    floorDeltaLabel_->setText(QStringLiteral("keep the signal source OFF while capturing"));
}

void MainWindow::finalizeFloorCapture()
{
    floorCapturing_ = false;
    if (floorCount_ == 0) {
        floorLabel_->setText(QStringLiteral("Floor: no data"));
        floorDeltaLabel_->clear();
        return;
    }
    floorDbm_ = floorSum_ / floorCount_;
    const double seconds = clock_.elapsed() / 1000.0 - floorStartedAt_;
    floorLabel_->setText(QStringLiteral("Floor: %1 dBm  (%2 s average)").arg(dbmText(floorDbm_)).arg(seconds, 0, 'f', 1));
    floorDeltaLabel_->clear();
    floorClearButton_->setEnabled(true);
}

void MainWindow::clearFloor()
{
    floorDbm_ = std::numeric_limits<double>::quiet_NaN();
    floorLabel_->setText(QStringLiteral("Floor: —"));
    floorDeltaLabel_->clear();
    floorClearButton_->setEnabled(false);
}

void MainWindow::updateFloorReadout(const SampleBin& bin, double avg)
{
    const double now = clock_.elapsed() / 1000.0;
    if (floorCapturing_) {
        floorSum_ += bin.sum;
        floorCount_ += bin.count;
        floorLabel_->setText(QStringLiteral("Floor: capturing… %1 s").arg(now - floorStartedAt_, 0, 'f', 1));
        if (floorEndAt_ && now >= *floorEndAt_) finalizeFloorCapture();
        return;
    }
    if (std::isnan(floorDbm_)) return;

    const double delta = avg - floorDbm_;
    const double net = rfpm::dbmToWatts(avg) - rfpm::dbmToWatts(floorDbm_);
    if (delta < 0.2 || net <= 0) {
        floorDeltaLabel_->setText(QStringLiteral("Δ %1 dB — at / below the floor").arg(dbmText(delta)));
    } else {
        floorDeltaLabel_->setText(QStringLiteral("Δ %1 dB above the floor — net %2 dBm (%3)")
                                      .arg(dbmText(delta), dbmText(rfpm::wattsToDbm(net)), wattsText(net)));
    }
}

// ── threshold events ────────────────────────────────────────────────────────────
// When armed and the level stays above the threshold for two consecutive ticks (~100 ms,
// debounces single noise spikes), log one timestamped event, then stay quiet until the
// level drops 2 dB below the threshold.

void MainWindow::processThresholdEvent(double avg)
{
    if (!eventsEnable_->isChecked()) {
        eventArmed_ = true;
        eventBinsAbove_ = 0;
        return;
    }
    const double threshold = eventsThreshold_->value();
    if (eventArmed_) {
        if (avg < threshold) {
            eventBinsAbove_ = 0;
        } else if (++eventBinsAbove_ >= 2) {
            eventArmed_ = false;
            eventBinsAbove_ = 0;
            eventsList_->insertItem(0, QStringLiteral("%1   %2 dBm")
                                           .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.z")))
                                           .arg(dbmText(avg)));
            while (eventsList_->count() > 200) delete eventsList_->takeItem(eventsList_->count() - 1);
            eventsStatus_->setText(QStringLiteral("%1 events — re-arms 2 dB below the threshold").arg(eventsList_->count()));
        }
    } else if (avg < threshold - 2.0) {
        eventArmed_ = true;
        eventsStatus_->setText(QStringLiteral("%1 events — armed").arg(eventsList_->count()));
    }
}

// ═════════════════════════════════════════════════════════════════════════════════
//  sweep view
// ═════════════════════════════════════════════════════════════════════════════════

void MainWindow::onSweep(const QVector<float>& sweep)
{
    if (!sweepRunning_) return;

    const int mode = trigModeCombo_->currentIndex();   // 0 Auto, 1 Normal, 2 Single
    if (mode != 0) {
        const auto& tb = rfpm::timebases()[timebaseIndex_];
        const int n = std::min<int>(tb.samples, sweep.size());
        const double level = trigLevelSpin_->value();
        const bool rising = trigEdgeCombo_->currentIndex() == 0;
        bool fired = false;
        for (int i = 1; i < n && !fired; ++i) {
            fired = rising ? (sweep[i - 1] < level && sweep[i] >= level) : (sweep[i - 1] > level && sweep[i] <= level);
        }
        if (!fired) {
            if (!waitingForTrigger_) {
                waitingForTrigger_ = true;
                updateSweepBadge();
            }
            return;
        }
    }
    waitingForTrigger_ = false;
    scope_->setSweep(sweep);
    ++sweepCount_;
    updateMarkerReadouts();
    if (mode == 2) setSweepRunning(false);
    else updateSweepBadge();
}

void MainWindow::setSweepRunning(bool running)
{
    sweepRunning_ = running;
    runButton_->setText(running ? QStringLiteral("Stop") : QStringLiteral("Run"));
    runButton_->setProperty("accent", !running);
    runButton_->style()->unpolish(runButton_);
    runButton_->style()->polish(runButton_);
    waitingForTrigger_ = false;
    updateSweepBadge();
}

void MainWindow::armSingle()
{
    if (trigModeCombo_->currentIndex() != 2) trigModeCombo_->setCurrentIndex(2);   // also sets running
    else setSweepRunning(true);
    tabs_->setCurrentIndex(1);
}

void MainWindow::updateSweepBadge()
{
    if (!scope_) return;
    const int mode = trigModeCombo_ ? trigModeCombo_->currentIndex() : 0;
    if (!link_.isOpen()) {
        scope_->setBadge(sweepRunning_ ? QString() : QStringLiteral("STOPPED"), theme::Bad);
    } else if (!sweepRunning_) {
        scope_->setBadge(mode == 2 && scope_->hasSweep() ? QStringLiteral("CAPTURED") : QStringLiteral("STOPPED"),
                         mode == 2 ? theme::Good : theme::Bad);
    } else if (mode != 0) {
        scope_->setBadge(waitingForTrigger_ || !scope_->hasSweep() ? QStringLiteral("ARMED · waiting for trigger")
                                                                   : QStringLiteral("TRIGGERED"),
                         theme::Warn);
    } else {
        scope_->setBadge(QStringLiteral("LIVE"), theme::AccentHot);
    }
}

void MainWindow::updateMarkerReadouts()
{
    if (!scope_) return;
    const auto a = scope_->markerInfo(0);
    const auto b = scope_->markerInfo(1);
    auto fmt = [](const SweepScope::MarkerInfo& m) {
        return QStringLiteral("%1  %2").arg(qs(rfpm::formatTime(m.time)),
                                            std::isnan(m.dbm) ? QStringLiteral("--.- dBm") : dbmText(m.dbm) + QStringLiteral(" dBm"));
    };
    m1Label_->setText(QStringLiteral("<span style='color:%1'>M1</span>  %2").arg(css(theme::Floor), fmt(a).toHtmlEscaped()));
    m2Label_->setText(QStringLiteral("<span style='color:%1'>M2</span>  %2").arg(css(theme::Peak), fmt(b).toHtmlEscaped()));
    const double dt = std::fabs(b.time - a.time);
    QString delta = QStringLiteral("ΔT %1   1/ΔT %2")
                        .arg(qs(rfpm::formatTime(dt)), dt > 0 ? qs(rfpm::formatSi(1.0 / dt, "Hz")) : QStringLiteral("--"));
    if (!std::isnan(a.dbm) && !std::isnan(b.dbm)) delta += QStringLiteral("   Δ %1 dB").arg(dbmText(b.dbm - a.dbm));
    deltaLabel_->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(css(theme::Text), delta.toHtmlEscaped()));

    const auto& s = scope_->sweep();
    const int n = scope_->visibleSamples();
    if (n > 0) {
        double sum = 0, mn = s[0], mx = s[0];
        for (int i = 0; i < n; ++i) {
            sum += s[i];
            mn = std::min<double>(mn, s[i]);
            mx = std::max<double>(mx, s[i]);
        }
        sweepStatsLabel_->setText(QStringLiteral("<span style='color:%1'>sweep #%2</span>  avg %3  max %4  min %5")
                                      .arg(css(theme::TextFaint))
                                      .arg(sweepCount_)
                                      .arg(dbmText(sum / n), dbmText(mx), dbmText(mn)));
    } else {
        sweepStatsLabel_->setText(QStringLiteral("<span style='color:%1'>no sweep</span>").arg(css(theme::TextFaint)));
    }
}

void MainWindow::syncViewControls()
{
    const QSignalBlocker b1(scaleCombo_);
    const QSignalBlocker b2(topSpin_);
    const int i = SweepScope::scales().indexOf(scope_->scale());
    scaleCombo_->setCurrentIndex(i >= 0 ? i : SweepScope::scales().indexOf(10.0));
    topSpin_->setSingleStep(scope_->scale());
    topSpin_->setValue(scope_->top());
}

void MainWindow::exportSweep()
{
    if (!scope_->hasSweep()) {
        QMessageBox::information(this, QStringLiteral("Export sweep"), QStringLiteral("There is no sweep to export yet."));
        return;
    }
    const QString suggested = QDir(documentsDir(QStringLiteral("sweeps")))
                                  .filePath(QStringLiteral("sweep_%1.csv").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export sweep"), suggested,
                                                      QStringLiteral("CSV (*.csv);;All files (*)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        appendLog(MeterLink::LogError, QStringLiteral("export failed: %1").arg(f.errorString()));
        return;
    }
    const auto& tb = rfpm::timebases()[timebaseIndex_];
    QTextStream out(&f);
    out << "Index,Time_s,dBm,Watt\n";
    const auto& s = scope_->sweep();
    for (int i = 0; i < s.size(); ++i) {
        out << i << ',' << QString::number(i * tb.periodSec, 'g', 9) << ',' << QString::number(s[i], 'f', 1) << ','
            << QString::number(rfpm::dbmToWatts(s[i]), 'E', 4) << '\n';
    }
    appendLog(MeterLink::LogInfo, QStringLiteral("Exported %1 samples (%2) to %3").arg(s.size()).arg(qs(tb.label), path));
}

void MainWindow::saveImage()
{
    QWidget* target = tabs_->currentIndex() == 1 ? static_cast<QWidget*>(scope_) : static_cast<QWidget*>(chart_);
    const QString name = tabs_->currentIndex() == 1 ? QStringLiteral("sweep") : QStringLiteral("monitor");
    const QString suggested = QDir(documentsDir(QStringLiteral("images")))
                                  .filePath(QStringLiteral("%1_%2.png").arg(name, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    const QPixmap shot = target->grab();   // grab first: the dialog must not end up in the image
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save image"), suggested, QStringLiteral("PNG (*.png)"));
    if (path.isEmpty()) return;
    if (shot.save(path)) appendLog(MeterLink::LogInfo, QStringLiteral("Saved %1").arg(path));
    else appendLog(MeterLink::LogError, QStringLiteral("could not save %1").arg(path));
}

// ═════════════════════════════════════════════════════════════════════════════════
//  log / CSV
// ═════════════════════════════════════════════════════════════════════════════════

void MainWindow::appendLog(int level, const QString& text)
{
    QColor color = theme::TextDim;
    switch (level) {
    case MeterLink::LogSent: color = theme::AccentHot; break;
    case MeterLink::LogReceived: color = theme::Good; break;
    case MeterLink::LogError: color = theme::Bad; break;
    default: break;
    }
    const QString line = level == MeterLink::LogError && !text.startsWith(QLatin1String("E-")) ? QStringLiteral("E-") + text : text;
    logView_->appendHtml(QStringLiteral("<span style='color:%1'>%2</span>&nbsp;&nbsp;<span style='color:%3'>%4</span>")
                             .arg(css(theme::TextFaint), QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                                  css(color), line.toHtmlEscaped()));
    if (level == MeterLink::LogError && tabs_->currentIndex() != 2) tabs_->setTabText(2, QStringLiteral("Log ●"));
}

void MainWindow::toggleCsvLog()
{
    if (csvFile_.isOpen()) {
        stopCsvLog();
        return;
    }
    const QString path = QDir(documentsDir(QStringLiteral("logs")))
                             .filePath(QStringLiteral("rf_%1.csv").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    csvFile_.setFileName(path);
    if (!csvFile_.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        csvStatus_->setText(QStringLiteral("log failed: %1").arg(csvFile_.errorString()));
        return;
    }
    csvStream_.setDevice(&csvFile_);
    csvStream_ << "timestamp,avg_dbm,min_dbm,max_dbm,samples\n";
    csvRows_ = 0;
    csvButton_->setText(QStringLiteral("Stop"));
    csvButton_->setProperty("danger", true);
    csvButton_->style()->unpolish(csvButton_);
    csvButton_->style()->polish(csvButton_);
    csvStatus_->setText(QStringLiteral("logging → %1").arg(QFileInfo(path).fileName()));
    appendLog(MeterLink::LogInfo, QStringLiteral("CSV log started: %1").arg(path));
}

void MainWindow::stopCsvLog()
{
    if (!csvFile_.isOpen()) return;
    csvStream_.flush();
    csvStream_.setDevice(nullptr);
    const QString name = QFileInfo(csvFile_.fileName()).fileName();
    csvFile_.close();
    if (csvButton_) {
        csvButton_->setText(QStringLiteral("Start"));
        csvButton_->setProperty("danger", false);
        csvButton_->style()->unpolish(csvButton_);
        csvButton_->style()->polish(csvButton_);
        csvStatus_->setText(QStringLiteral("saved %L1 rows: %2").arg(csvRows_).arg(name));
    }
}

void MainWindow::openLogFolder()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(documentsDir(QStringLiteral("logs"))));
}

// ═════════════════════════════════════════════════════════════════════════════════
//  settings
// ═════════════════════════════════════════════════════════════════════════════════

void MainWindow::loadSettings()
{
    QSettings s;
    restoreGeometry(s.value(QStringLiteral("window/geometry")).toByteArray());

    const int baud = s.value(QStringLiteral("connection/baud"), rfpm::kDefaultBaud).toInt();
    bool found = false;
    for (QAction* a : baudGroup_->actions()) {
        if (a->data().toInt() == baud) {
            a->setChecked(true);
            found = true;
        }
    }
    if (!found && baud > 0) {   // a custom rate from the .ini file
        QAction* a = baudMenu_->addAction(QString::number(baud));
        a->setCheckable(true);
        a->setData(baud);
        baudGroup_->addAction(a);
        a->setChecked(true);
    }
    actDtr_->setChecked(s.value(QStringLiteral("connection/assertDtr"), true).toBool());
    actAutoConnect_->setChecked(s.value(QStringLiteral("connection/autoConnect"), false).toBool());
    actAutoReconnect_->setChecked(s.value(QStringLiteral("connection/autoReconnect"), true).toBool());

    freqSpin_->setValue(s.value(QStringLiteral("meter/frequency"), 1000).toInt());
    offsetSpin_->setValue(s.value(QStringLiteral("meter/offset"), 0.0).toDouble());
    restoreCheck_->setChecked(s.value(QStringLiteral("meter/restoreOnConnect"), true).toBool());

    windowCombo_->setCurrentIndex(std::clamp(s.value(QStringLiteral("monitor/window"), 2).toInt(), 0, 4));
    maxTraceCheck_->setChecked(s.value(QStringLiteral("monitor/maxTrace"), true).toBool());
    eventsThreshold_->setValue(s.value(QStringLiteral("events/threshold"), -40.0).toDouble());

    scope_->setView(s.value(QStringLiteral("sweep/scale"), 10.0).toDouble(), s.value(QStringLiteral("sweep/top"), 10.0).toDouble());
    syncViewControls();
    trigLevelSpin_->setValue(s.value(QStringLiteral("sweep/triggerLevel"), -40.0).toDouble());
    scope_->setTriggerLevel(trigLevelSpin_->value());
    trigEdgeCombo_->setCurrentIndex(std::clamp(s.value(QStringLiteral("sweep/triggerEdge"), 0).toInt(), 0, 1));
    trigModeCombo_->setCurrentIndex(std::clamp(s.value(QStringLiteral("sweep/triggerMode"), 0).toInt(), 0, 2));
    scope_->setTriggerArmed(trigModeCombo_->currentIndex() != 0);
    scope_->setMarkers(s.value(QStringLiteral("sweep/marker1"), 0.15).toDouble(), s.value(QStringLiteral("sweep/marker2"), 0.55).toDouble());
    setTimebaseIndex(s.value(QStringLiteral("sweep/timebase"), rfpm::defaultTimebaseIndex()).toInt(), false);

    tabs_->setCurrentIndex(std::clamp(s.value(QStringLiteral("window/tab"), 0).toInt(), 0, 2));
}

void MainWindow::saveSettings()
{
    QSettings s;
    s.setValue(QStringLiteral("window/geometry"), saveGeometry());
    s.setValue(QStringLiteral("window/tab"), tabs_->currentIndex());
    if (!selectedPort().isEmpty()) s.setValue(QStringLiteral("connection/port"), selectedPort());
    s.setValue(QStringLiteral("connection/baud"), selectedBaud());
    s.setValue(QStringLiteral("connection/assertDtr"), actDtr_->isChecked());
    s.setValue(QStringLiteral("connection/autoConnect"), actAutoConnect_->isChecked());
    s.setValue(QStringLiteral("connection/autoReconnect"), actAutoReconnect_->isChecked());
    s.setValue(QStringLiteral("meter/frequency"), freqSpin_->value());
    s.setValue(QStringLiteral("meter/offset"), offsetSpin_->value());
    s.setValue(QStringLiteral("meter/restoreOnConnect"), restoreCheck_->isChecked());
    s.setValue(QStringLiteral("monitor/window"), windowCombo_->currentIndex());
    s.setValue(QStringLiteral("monitor/maxTrace"), maxTraceCheck_->isChecked());
    s.setValue(QStringLiteral("events/threshold"), eventsThreshold_->value());
    s.setValue(QStringLiteral("sweep/scale"), scope_->scale());
    s.setValue(QStringLiteral("sweep/top"), scope_->top());
    s.setValue(QStringLiteral("sweep/triggerLevel"), trigLevelSpin_->value());
    s.setValue(QStringLiteral("sweep/triggerEdge"), trigEdgeCombo_->currentIndex());
    s.setValue(QStringLiteral("sweep/triggerMode"), trigModeCombo_->currentIndex());
    s.setValue(QStringLiteral("sweep/marker1"), scope_->markerFraction(0));
    s.setValue(QStringLiteral("sweep/marker2"), scope_->markerFraction(1));
    s.setValue(QStringLiteral("sweep/timebase"), timebaseIndex_);
}
