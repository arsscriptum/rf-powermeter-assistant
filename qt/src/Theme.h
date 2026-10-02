// ╔════════════════════════════════════════════════════════════════════════════════╗
// ║   Theme.h                                                                      ║
// ║   "Nebula purple" theme: palette, style sheet, fonts, app icon                 ║
// ╚════════════════════════════════════════════════════════════════════════════════╝

#pragma once

#include <QColor>
#include <QFont>
#include <QIcon>

class QApplication;
class QWidget;

namespace theme {

// Palette
inline const QColor BgDeep{0x0C, 0x08, 0x13};
inline const QColor Bg{0x12, 0x0D, 0x1D};
inline const QColor Card{0x1A, 0x13, 0x29};
inline const QColor ChartBg{0x13, 0x0C, 0x22};
inline const QColor Field{0x17, 0x0F, 0x2A};
inline const QColor Stroke{0x2E, 0x21, 0x50};
inline const QColor StrokeBright{0x4C, 0x3A, 0x82};
inline const QColor Grid{0x26, 0x1C, 0x42};
inline const QColor Accent{0xA8, 0x55, 0xF7};
inline const QColor AccentHot{0xC0, 0x84, 0xFC};
inline const QColor AccentDeep{0x7C, 0x3A, 0xED};
inline const QColor Pink{0xE8, 0x79, 0xF9};
inline const QColor Text{0xED, 0xE6, 0xFA};
inline const QColor TextDim{0x9D, 0x8F, 0xC2};
inline const QColor TextFaint{0x6E, 0x5F, 0x96};
inline const QColor Neon{0xD8, 0xB4, 0xFE};
inline const QColor Watts{0xA7, 0x8B, 0xFA};
inline const QColor Peak{0xF0, 0xAB, 0xFC};
inline const QColor Floor{0x7D, 0xD3, 0xFC};
inline const QColor Good{0x86, 0xEF, 0xAC};
inline const QColor Bad{0xF8, 0x71, 0x71};
inline const QColor Warn{0xF5, 0x9E, 0x0B};
inline const QColor Idle{0x5B, 0x55, 0x75};

/// Signal-strength color scale shared by every trace: violet floor -> blue -> amber -> orange -> red.
QColor colorForDbm(double dbm);
struct Stop { double dbm; QColor color; };
const Stop* strengthStops(int* count);

/// Fusion style + dark palette + style sheet + fonts.
void apply(QApplication& app);

QFont uiFont(double pointSize = -1, QFont::Weight weight = QFont::Normal);
QFont monoFont(double pointSize, bool bold = false);

/// The app icon, painted (no image assets).
QIcon appIcon();
void paintLogo(class QPainter& p, const class QRectF& r);

/// Tag a QFrame as a card: role = "card" | "hero" | "bar".
void makeCard(QWidget* w, const char* role = "card");

/// Windows 10/11: dark native title bar matching the theme. No-op elsewhere.
void applyNativeTitleBar(QWidget* window);

}  // namespace theme
