// ╔════════════════════════════════════════════════════════════════════════════════╗
// ║   Theme.cpp                                                                    ║
// ╚════════════════════════════════════════════════════════════════════════════════╝

#include "Theme.h"

#include <algorithm>

#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOption>
#include <QTemporaryDir>
#include <QWidget>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

namespace theme {

// ─── strength scale ─────────────────────────────────────────────────────────────

static const Stop kStops[] = {
    {-95, QColor(0x8B, 0x5C, 0xF6)},   // violet: noise floor
    {-70, QColor(0x3B, 0x82, 0xF6)},   // blue
    {-45, QColor(0xF5, 0x9E, 0x0B)},   // amber
    {-25, QColor(0xF9, 0x73, 0x16)},   // orange
    {-10, QColor(0xEF, 0x44, 0x44)},   // red: hot
};

const Stop* strengthStops(int* count)
{
    *count = int(sizeof kStops / sizeof kStops[0]);
    return kStops;
}

QColor colorForDbm(double dbm)
{
    const int n = int(sizeof kStops / sizeof kStops[0]);
    if (dbm <= kStops[0].dbm) return kStops[0].color;
    for (int i = 1; i < n; ++i) {
        if (dbm <= kStops[i].dbm) {
            const double t = (dbm - kStops[i - 1].dbm) / (kStops[i].dbm - kStops[i - 1].dbm);
            const QColor& a = kStops[i - 1].color;
            const QColor& b = kStops[i].color;
            return QColor(int(a.red() + (b.red() - a.red()) * t), int(a.green() + (b.green() - a.green()) * t),
                          int(a.blue() + (b.blue() - a.blue()) * t));
        }
    }
    return kStops[n - 1].color;
}

// ─── fonts ──────────────────────────────────────────────────────────────────────

static QStringList installedFamilies()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return QFontDatabase::families();
#else
    return QFontDatabase().families();
#endif
}

static QString firstInstalled(const QStringList& wanted, const QString& fallback)
{
    static const QStringList families = installedFamilies();
    for (const QString& f : wanted) {
        if (families.contains(f, Qt::CaseInsensitive)) return f;
    }
    return fallback;
}

QFont uiFont(double pointSize, QFont::Weight weight)
{
    static const QString family = firstInstalled(
        {QStringLiteral("Segoe UI"), QStringLiteral("Inter"), QStringLiteral("Noto Sans"), QStringLiteral("Ubuntu"),
         QStringLiteral("Cantarell"), QStringLiteral("DejaVu Sans")},
        QApplication::font().family());
    QFont f(family);
    f.setPointSizeF(pointSize > 0 ? pointSize : 9.75);
    f.setWeight(weight);
    return f;
}

QFont monoFont(double pointSize, bool bold)
{
    static const QString family = firstInstalled(
        {QStringLiteral("Consolas"), QStringLiteral("Cascadia Mono"), QStringLiteral("JetBrains Mono"),
         QStringLiteral("DejaVu Sans Mono"), QStringLiteral("Liberation Mono"), QStringLiteral("Noto Sans Mono")},
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
    QFont f(family);
    f.setStyleHint(QFont::Monospace);
    f.setPointSizeF(pointSize);
    f.setBold(bold);
    return f;
}

// ─── style: chevrons and check boxes drawn in code, no image assets ─────────────

namespace {

class NebulaStyle : public QProxyStyle
{
public:
    NebulaStyle()
        : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion")))
    {
    }

    void drawPrimitive(PrimitiveElement pe, const QStyleOption* opt, QPainter* p, const QWidget* w) const override
    {
        switch (pe) {
        case PE_IndicatorArrowDown:
        case PE_IndicatorArrowUp:
        case PE_IndicatorArrowLeft:
        case PE_IndicatorArrowRight:
        case PE_IndicatorSpinUp:
        case PE_IndicatorSpinDown:
        case PE_IndicatorSpinPlus:
        case PE_IndicatorSpinMinus:
            drawChevron(pe, opt, p);
            return;
        case PE_IndicatorCheckBox:
            drawCheckBox(opt, p);
            return;
        case PE_FrameFocusRect:
            return;   // focus is shown by the accent border instead
        default:
            QProxyStyle::drawPrimitive(pe, opt, p, w);
        }
    }

    int pixelMetric(PixelMetric m, const QStyleOption* opt, const QWidget* w) const override
    {
        if (m == PM_IndicatorWidth || m == PM_IndicatorHeight) return 17;
        if (m == PM_CheckBoxLabelSpacing) return 7;
        return QProxyStyle::pixelMetric(m, opt, w);
    }

    int styleHint(StyleHint hint, const QStyleOption* opt, const QWidget* w, QStyleHintReturn* ret) const override
    {
        if (hint == SH_ComboBox_Popup) return 0;   // drop-down list below the box, not over it
        return QProxyStyle::styleHint(hint, opt, w, ret);
    }

private:
    static void drawChevron(PrimitiveElement pe, const QStyleOption* opt, QPainter* p)
    {
        const QRectF r = opt->rect;
        const bool enabled = opt->state & State_Enabled;
        const bool hot = opt->state & (State_MouseOver | State_Sunken);
        QColor c = hot ? AccentHot : TextDim;
        if (!enabled) c = Idle;
        const double s = std::min(r.width(), r.height()) * 0.22 + 1.6;
        const QPointF m = r.center();
        QPainterPath path;
        switch (pe) {
        case PE_IndicatorArrowUp:
        case PE_IndicatorSpinUp:
        case PE_IndicatorSpinPlus:
            path.moveTo(m.x() - s, m.y() + s / 2);
            path.lineTo(m.x(), m.y() - s / 2);
            path.lineTo(m.x() + s, m.y() + s / 2);
            break;
        case PE_IndicatorArrowLeft:
            path.moveTo(m.x() + s / 2, m.y() - s);
            path.lineTo(m.x() - s / 2, m.y());
            path.lineTo(m.x() + s / 2, m.y() + s);
            break;
        case PE_IndicatorArrowRight:
            path.moveTo(m.x() - s / 2, m.y() - s);
            path.lineTo(m.x() + s / 2, m.y());
            path.lineTo(m.x() - s / 2, m.y() + s);
            break;
        default:
            path.moveTo(m.x() - s, m.y() - s / 2);
            path.lineTo(m.x(), m.y() + s / 2);
            path.lineTo(m.x() + s, m.y() - s / 2);
            break;
        }
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(QPen(c, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->setBrush(Qt::NoBrush);
        p->drawPath(path);
        p->restore();
    }

    static void drawCheckBox(const QStyleOption* opt, QPainter* p)
    {
        const QRectF r = QRectF(opt->rect).adjusted(0.7, 0.7, -0.7, -0.7);
        const bool on = opt->state & State_On;
        const bool hot = opt->state & State_MouseOver;
        const bool enabled = opt->state & State_Enabled;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        if (!enabled) p->setOpacity(0.4);
        if (on) {
            QLinearGradient g(r.topLeft(), r.topRight());
            g.setColorAt(0, AccentDeep);
            g.setColorAt(0.55, Accent);
            g.setColorAt(1, Pink);
            p->setBrush(g);
            p->setPen(QPen(Accent, 1.4));
        } else {
            p->setBrush(Field);
            p->setPen(QPen(hot ? AccentHot : StrokeBright, 1.4));
        }
        p->drawRoundedRect(r, 5, 5);
        if (on) {
            QPainterPath check;
            check.moveTo(r.left() + r.width() * 0.24, r.top() + r.height() * 0.52);
            check.lineTo(r.left() + r.width() * 0.43, r.top() + r.height() * 0.72);
            check.lineTo(r.left() + r.width() * 0.77, r.top() + r.height() * 0.30);
            p->setPen(QPen(Qt::white, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p->setBrush(Qt::NoBrush);
            p->drawPath(check);
        }
        p->restore();
    }
};

const char* kStyleSheet = R"QSS(
QMainWindow, QDialog {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
        stop:0 #16102A, stop:0.45 #0C0813, stop:0.8 #0E0A18, stop:1 #1B0F33);
}
QWidget { color: #EDE6FA; }
QLabel { background: transparent; }

QFrame[card="card"] {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #26203B, stop:1 #1A1329);
    border: 1px solid #2E2150;
    border-radius: 14px;
}
QFrame[card="hero"] {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #1D1438, stop:1 #150E2B);
    border: 1px solid #4C3A82;
    border-radius: 14px;
}
QFrame[card="bar"] {
    background: #221840;
    border: 1px solid #2E2150;
    border-radius: 12px;
}
QFrame[card="strip"] {
    background: #160F28;
    border: 1px solid #2E2150;
    border-radius: 10px;
}

QLabel[role="section"] { color: #C084FC; font-weight: 600; }
QLabel[role="caption"] { color: #6E5F96; font-weight: 600; }
QLabel[role="dim"]     { color: #9D8FC2; }
QLabel[role="faint"]   { color: #6E5F96; }
QLabel[role="hint"]    { color: #6E5F96; }

/* ── buttons ── */
QPushButton, QToolButton {
    color: #EDE6FA;
    background: #241A3D;
    border: 1px solid #2E2150;
    border-radius: 8px;
    padding: 5px 12px;
    font-weight: 500;
}
QPushButton:hover, QToolButton:hover { background: #33255C; border-color: #A855F7; }
QPushButton:pressed, QToolButton:pressed { background: #1C1433; }
QPushButton:checked, QToolButton:checked { background: #33255C; border-color: #C084FC; color: #FFFFFF; }
QPushButton:disabled, QToolButton:disabled { color: #5B5575; background: #1A1430; border-color: #241B3C; }
QToolButton::menu-indicator { image: none; width: 0px; }
QToolButton[popupMode="2"] { padding-right: 12px; }

QPushButton[accent="true"] {
    color: #FFFFFF;
    font-weight: 600;
    border: 1px solid #9B5CF6;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #7C3AED, stop:0.55 #A855F7, stop:1 #E879F9);
}
QPushButton[accent="true"]:hover {
    border-color: #F0ABFC;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #8B5CF6, stop:0.55 #C084FC, stop:1 #F0ABFC);
}
QPushButton[accent="true"]:pressed { background: #7C3AED; }
QPushButton[danger="true"] { border-color: #7F1D1D; }
QPushButton[danger="true"]:hover { background: #4C1D2E; border-color: #F87171; }

/* ── inputs ── */
QLineEdit, QAbstractSpinBox, QComboBox {
    color: #EDE6FA;
    background: #170F2A;
    border: 1px solid #2E2150;
    border-radius: 8px;
    padding: 4px 8px;
    min-height: 20px;
    selection-background-color: #7C3AED;
    selection-color: #FFFFFF;
}
QLineEdit:hover, QAbstractSpinBox:hover, QComboBox:hover { border-color: #4C3A82; }
QLineEdit:focus, QAbstractSpinBox:focus, QComboBox:focus, QComboBox:on { border-color: #A855F7; }
QLineEdit:disabled, QAbstractSpinBox:disabled, QComboBox:disabled { color: #5B5575; border-color: #241B3C; }
QAbstractSpinBox { padding-right: 20px; }
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button {
    subcontrol-origin: border;
    width: 18px;
    border: none;
    background: transparent;
}
QAbstractSpinBox::up-button { subcontrol-position: top right; margin: 2px 2px 0 0; }
QAbstractSpinBox::down-button { subcontrol-position: bottom right; margin: 0 2px 2px 0; }
QAbstractSpinBox::up-button:hover, QAbstractSpinBox::down-button:hover { background: #2A1E4A; border-radius: 4px; }
QComboBox { padding-right: 26px; }
QComboBox::drop-down {
    subcontrol-origin: padding;
    subcontrol-position: center right;
    width: 24px;
    border: none;
    background: transparent;
}
QComboBox::down-arrow { image: url(%ICONS%/chev-down.png); width: 12px; height: 12px; }
QComboBox::down-arrow:hover, QComboBox::down-arrow:on { image: url(%ICONS%/chev-down-hot.png); }
QComboBox::down-arrow:disabled { image: url(%ICONS%/chev-down-off.png); }
QAbstractSpinBox::up-arrow { image: url(%ICONS%/chev-up.png); width: 12px; height: 12px; }
QAbstractSpinBox::down-arrow { image: url(%ICONS%/chev-down.png); width: 12px; height: 12px; }
QAbstractSpinBox::up-arrow:hover { image: url(%ICONS%/chev-up-hot.png); }
QAbstractSpinBox::down-arrow:hover { image: url(%ICONS%/chev-down-hot.png); }
QAbstractSpinBox::up-arrow:disabled, QAbstractSpinBox::up-arrow:off { image: url(%ICONS%/chev-up-off.png); }
QAbstractSpinBox::down-arrow:disabled, QAbstractSpinBox::down-arrow:off { image: url(%ICONS%/chev-down-off.png); }
QComboBox QAbstractItemView {
    color: #EDE6FA;
    background: #1C1331;
    border: 1px solid #4C3A82;
    padding: 4px;
    outline: none;
    selection-background-color: #7C3AED;
}
QComboBox QAbstractItemView::item { min-height: 24px; padding: 2px 8px; border-radius: 6px; }
QComboBox QAbstractItemView::item:hover { background: #3A2A66; }

QCheckBox { spacing: 7px; background: transparent; }
QCheckBox:disabled { color: #5B5575; }

/* ── lists and text ── */
QListWidget, QPlainTextEdit, QTextEdit {
    color: #EDE6FA;
    background: #170F2A;
    border: 1px solid #2E2150;
    border-radius: 8px;
    padding: 2px;
    selection-background-color: #7C3AED;
}
QListWidget::item { padding: 2px 6px; border-radius: 5px; }
QListWidget::item:hover { background: #2A1E4A; }
QListWidget::item:selected { background: #7C3AED; color: #FFFFFF; }

/* ── scroll bars ── */
QScrollArea { background: transparent; border: none; }
QScrollBar:vertical { background: transparent; width: 9px; margin: 2px 0 2px 0; }
QScrollBar:horizontal { background: transparent; height: 9px; margin: 0 2px 0 2px; }
QScrollBar::handle { background: #33275A; border-radius: 3px; margin: 1px 2px; }
QScrollBar::handle:hover { background: #5B45A0; }
QScrollBar::handle:vertical { min-height: 28px; }
QScrollBar::handle:horizontal { min-width: 28px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; border: none; background: none; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

/* ── tabs ── */
QTabWidget::pane { border: none; background: transparent; }
QTabWidget::tab-bar { left: 2px; }
QTabBar { background: transparent; }
QTabBar::tab {
    color: #9D8FC2;
    background: transparent;
    border: 1px solid transparent;
    border-radius: 8px;
    padding: 6px 16px;
    margin: 0 6px 6px 0;
    font-weight: 600;
}
QTabBar::tab:hover { background: #241A3D; }
QTabBar::tab:selected { color: #EDE6FA; background: #33255C; border-color: #A855F7; }

/* ── menus and tooltips ── */
QMenu {
    color: #EDE6FA;
    background: #1C1331;
    border: 1px solid #4C3A82;
    padding: 5px;
}
QMenu::item { padding: 6px 22px 6px 10px; border-radius: 6px; background: transparent; }
QMenu::item:selected { background: #3A2A66; }
QMenu::item:disabled { color: #5B5575; }
QMenu::separator { height: 1px; background: #2E2150; margin: 5px 8px; }
QMenu::indicator { width: 15px; height: 15px; margin-left: 4px; }
QToolTip {
    color: #EDE6FA;
    background: #221840;
    border: 1px solid #4C3A82;
    padding: 5px 8px;
}
QSplitter::handle { background: transparent; }
)QSS";

}  // namespace

// ─── chevron images for the style sheet ─────────────────────────────────────────
// The style-sheet engine draws combo / spin arrows only from images, so render a few small
// chevrons once at startup (1x and @2x for high-DPI screens) into a temporary folder.

static void paintChevron(QPainter& p, const QRectF& r, bool up, const QColor& c)
{
    const double s = r.width() * 0.3;
    const QPointF m = r.center();
    const double d = up ? -1 : 1;
    QPainterPath path;
    path.moveTo(m.x() - s, m.y() - d * s / 2);
    path.lineTo(m.x(), m.y() + d * s / 2);
    path.lineTo(m.x() + s, m.y() - d * s / 2);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(c, r.width() / 7.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

static QString writeChevrons()
{
    static QTemporaryDir dir(QDir::tempPath() + QStringLiteral("/rfpm-theme-XXXXXX"));
    if (!dir.isValid()) return QString();
    const struct { const char* suffix; QColor color; } variants[] = {
        {"", TextDim}, {"-hot", AccentHot}, {"-off", Idle},
    };
    for (bool up : {true, false}) {
        for (const auto& v : variants) {
            for (int scale : {1, 2}) {
                QImage img(12 * scale, 12 * scale, QImage::Format_ARGB32_Premultiplied);
                img.fill(Qt::transparent);
                QPainter p(&img);
                paintChevron(p, QRectF(0, 0, 12.0 * scale, 12.0 * scale), up, v.color);
                p.end();
                const QString name = QStringLiteral("chev-%1%2%3.png")
                                         .arg(up ? QStringLiteral("up") : QStringLiteral("down"), QString::fromLatin1(v.suffix),
                                              scale == 2 ? QStringLiteral("@2x") : QString());
                img.save(dir.filePath(name));
            }
        }
    }
    return dir.path();
}

// ─── apply ──────────────────────────────────────────────────────────────────────

void apply(QApplication& app)
{
    app.setStyle(new NebulaStyle);

    QPalette pal;
    pal.setColor(QPalette::Window, Bg);
    pal.setColor(QPalette::WindowText, Text);
    pal.setColor(QPalette::Base, Field);
    pal.setColor(QPalette::AlternateBase, Card);
    pal.setColor(QPalette::Text, Text);
    pal.setColor(QPalette::Button, QColor(0x24, 0x1A, 0x3D));
    pal.setColor(QPalette::ButtonText, Text);
    pal.setColor(QPalette::BrightText, Qt::white);
    pal.setColor(QPalette::Highlight, AccentDeep);
    pal.setColor(QPalette::HighlightedText, Qt::white);
    pal.setColor(QPalette::ToolTipBase, QColor(0x22, 0x18, 0x40));
    pal.setColor(QPalette::ToolTipText, Text);
    pal.setColor(QPalette::Link, AccentHot);
    pal.setColor(QPalette::PlaceholderText, TextFaint);
    pal.setColor(QPalette::Light, StrokeBright);
    pal.setColor(QPalette::Midlight, Stroke);
    pal.setColor(QPalette::Mid, Stroke);
    pal.setColor(QPalette::Dark, BgDeep);
    pal.setColor(QPalette::Shadow, Qt::black);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        pal.setColor(QPalette::Disabled, role, Idle);
    }
    app.setPalette(pal);
    app.setFont(uiFont());
    QString qss = QString::fromUtf8(kStyleSheet);
    qss.replace(QStringLiteral("%ICONS%"), writeChevrons());
    app.setStyleSheet(qss);
}

void makeCard(QWidget* w, const char* role)
{
    w->setProperty("card", QString::fromLatin1(role));
    w->setAttribute(Qt::WA_StyledBackground, true);
}

// ─── icon ───────────────────────────────────────────────────────────────────────

void paintLogo(QPainter& p, const QRectF& r)
{
    // A three-lobe RF wave, neon violet with a soft glow
    QPainterPath wave;
    const double w = r.width(), h = r.height();
    const double y0 = r.center().y();
    const double amp = h * 0.36;
    wave.moveTo(r.left(), y0);
    for (int i = 0; i < 4; ++i) {
        const double x0 = r.left() + w * i / 4.0;
        const double sign = (i % 2 == 0) ? -1.0 : 1.0;
        wave.quadTo(x0 + w / 8.0, y0 + sign * amp * 1.6, x0 + w / 4.0, y0);   // peak = half the control offset
    }
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    for (int i = 3; i >= 1; --i) {
        QColor glow = Accent;
        glow.setAlphaF(0.12f);
        p.setPen(QPen(glow, h * 0.09 + i * h * 0.07, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(wave);
    }
    p.setPen(QPen(AccentHot, std::max(1.6, h * 0.11), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(wave);
    p.restore();
}

QIcon appIcon()
{
    QIcon icon;
    for (int size : {16, 24, 32, 48, 64, 128, 256}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r(0.5, 0.5, size - 1.0, size - 1.0);
        QLinearGradient bg(r.topLeft(), r.bottomRight());
        bg.setColorAt(0, QColor(0x2A, 0x1B, 0x52));
        bg.setColorAt(1, QColor(0x0C, 0x08, 0x13));
        p.setBrush(bg);
        p.setPen(QPen(StrokeBright, std::max(1.0, size / 48.0)));
        p.drawRoundedRect(r, size * 0.22, size * 0.22);
        paintLogo(p, r.adjusted(size * 0.16, size * 0.3, -size * 0.16, -size * 0.3));
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

// ─── native title bar ───────────────────────────────────────────────────────────

void applyNativeTitleBar(QWidget* window)
{
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    const BOOL dark = TRUE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE: 20 on Windows 10 20H1+ / 11, 19 on older 10 builds
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof dark))) DwmSetWindowAttribute(hwnd, 19, &dark, sizeof dark);
    // Windows 11 only (ignored on 10): caption and border colors
    const COLORREF caption = RGB(0x14, 0x0E, 0x22);
    const COLORREF border = RGB(0x2E, 0x21, 0x50);
    const COLORREF text = RGB(0xED, 0xE6, 0xFA);
    DwmSetWindowAttribute(hwnd, 35, &caption, sizeof caption);   // DWMWA_CAPTION_COLOR
    DwmSetWindowAttribute(hwnd, 34, &border, sizeof border);     // DWMWA_BORDER_COLOR
    DwmSetWindowAttribute(hwnd, 36, &text, sizeof text);         // DWMWA_TEXT_COLOR
#else
    Q_UNUSED(window);
#endif
}

}  // namespace theme
