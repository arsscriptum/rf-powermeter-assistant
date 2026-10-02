// ╔════════════════════════════════════════════════════════════════════════════════╗
// ║   Widgets.cpp                                                                  ║
// ╚════════════════════════════════════════════════════════════════════════════════╝

#include "Widgets.h"

#include "Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QSpacerItem>

#include <algorithm>
#include <cmath>

// ─── GlowReadout ────────────────────────────────────────────────────────────────

GlowReadout::GlowReadout(QWidget* parent)
    : QWidget(parent)
{
    font_ = theme::monoFont(40, true);
    setAttribute(Qt::WA_TranslucentBackground);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // Slow breathing glow, like the companion's readout
    breathe_.setStartValue(0.35);
    breathe_.setKeyValueAt(0.5, 0.85);
    breathe_.setEndValue(0.35);
    breathe_.setDuration(3200);
    breathe_.setLoopCount(-1);
    connect(&breathe_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        const double o = v.toDouble();
        if (std::fabs(o - glowOpacity_) < 0.01) return;
        glowOpacity_ = o;
        update();
    });
    breathe_.start();
}

QSize GlowReadout::sizeHint() const
{
    return {260, QFontMetrics(font_).height() + 16};
}

void GlowReadout::setText(const QString& text)
{
    if (text == text_) return;
    text_ = text;
    glowDirty_ = true;
    update();
}

void GlowReadout::resizeEvent(QResizeEvent*)
{
    glowDirty_ = true;
}

void GlowReadout::rebuildGlow()
{
    glowDirty_ = false;
    // Cheap blur: draw the text at 1/4 size, box-blur it, then scale it back up smoothly.
    constexpr int f = 4;
    const int w = width() / f + 8, h = height() / f + 8;
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.translate(4, 4);
        p.scale(1.0 / f, 1.0 / f);
        p.setFont(font_);
        p.setPen(theme::Accent);
        p.drawText(rect(), Qt::AlignCenter, text_);
    }
    // two box-blur passes, radius 1, on premultiplied ARGB
    for (int pass = 0; pass < 2; ++pass) {
        QImage src = img.copy();
        for (int y = 0; y < h; ++y) {
            auto* out = reinterpret_cast<QRgb*>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                int a = 0, r = 0, g = 0, b = 0, n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    const int yy = y + dy;
                    if (yy < 0 || yy >= h) continue;
                    const auto* row = reinterpret_cast<const QRgb*>(src.constScanLine(yy));
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = x + dx;
                        if (xx < 0 || xx >= w) continue;
                        const QRgb px = row[xx];
                        a += qAlpha(px);
                        r += qRed(px);
                        g += qGreen(px);
                        b += qBlue(px);
                        ++n;
                    }
                }
                out[x] = qRgba(r / n, g / n, b / n, std::min(255, a * 2 / n));
            }
        }
    }
    glow_ = img;
}

void GlowReadout::paintEvent(QPaintEvent*)
{
    if (glowDirty_) rebuildGlow();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setOpacity(glowOpacity_);
    constexpr int f = 4;
    p.drawImage(QRectF(-4.0 * f, -4.0 * f, glow_.width() * f, glow_.height() * f), glow_);
    p.setOpacity(1.0);
    p.setFont(font_);
    p.setPen(theme::Neon);
    p.drawText(rect(), Qt::AlignCenter, text_);
}

// ─── SignalBar ──────────────────────────────────────────────────────────────────

SignalBar::SignalBar(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(14);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void SignalBar::setRange(double lo, double hi)
{
    lo_ = lo;
    hi_ = hi;
    update();
}

void SignalBar::setValue(double v)
{
    value_ = v;
    update();
}

void SignalBar::setPeak(double v)
{
    peak_ = v;
    update();
}

void SignalBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(theme::Stroke, 1));
    p.setBrush(QColor(0x17, 0x10, 0x28));
    p.drawRoundedRect(r, 6, 6);

    auto frac = [this](double v) { return std::clamp((v - lo_) / (hi_ - lo_), 0.0, 1.0); };
    const QRectF inner = r.adjusted(1.5, 1.5, -1.5, -1.5);

    if (!std::isnan(value_)) {
        const double w = inner.width() * frac(value_);
        if (w > 1) {
            QLinearGradient g(inner.topLeft(), inner.topRight());
            g.setColorAt(0, theme::AccentDeep);
            g.setColorAt(0.55, theme::Accent);
            g.setColorAt(1, theme::Pink);
            QRectF fill(inner.left(), inner.top(), w, inner.height());
            // soft glow
            QColor glow = theme::Accent;
            for (int i = 3; i >= 1; --i) {
                glow.setAlphaF(0.08f);
                p.setPen(Qt::NoPen);
                p.setBrush(glow);
                p.drawRoundedRect(fill.adjusted(-i, -i * 0.6, i, i * 0.6), 6, 6);
            }
            p.setBrush(g);
            p.drawRoundedRect(fill, 5, 5);
            // highlight
            p.setPen(QPen(QColor(255, 255, 255, 45), 1));
            p.drawLine(QPointF(fill.left() + 4, fill.top() + 1.5), QPointF(fill.right() - 4, fill.top() + 1.5));
        }
    }
    if (!std::isnan(peak_) && peak_ > lo_) {
        const double x = inner.left() + inner.width() * frac(peak_);
        p.setPen(QPen(theme::Peak, 2));
        p.drawLine(QPointF(x, r.top() + 1), QPointF(x, r.bottom() - 1));
    }
}

// ─── StatusDot ──────────────────────────────────────────────────────────────────

StatusDot::StatusDot(QWidget* parent)
    : QWidget(parent)
    , color_(theme::Idle)
{
    setFixedSize(12, 12);
}

void StatusDot::setColor(const QColor& c)
{
    color_ = c;
    update();
}

void StatusDot::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QColor halo = color_;
    halo.setAlphaF(0.25);
    p.setPen(Qt::NoPen);
    p.setBrush(halo);
    p.drawEllipse(QRectF(0, 0, 12, 12));
    p.setBrush(color_);
    p.drawEllipse(QRectF(2, 2, 8, 8));
}

// ─── FlowLayout ─────────────────────────────────────────────────────────────────

FlowLayout::FlowLayout(QWidget* parent, int hSpacing, int vSpacing)
    : QLayout(parent)
    , hSpace_(hSpacing)
    , vSpace_(vSpacing)
{
    setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout()
{
    while (QLayoutItem* item = takeAt(0)) delete item;
}

void FlowLayout::addItem(QLayoutItem* item)
{
    items_.append(item);
}

void FlowLayout::addSpacing(int px)
{
    addItem(new QSpacerItem(px, 1, QSizePolicy::Fixed, QSizePolicy::Minimum));
}

QLayoutItem* FlowLayout::takeAt(int index)
{
    return (index >= 0 && index < items_.size()) ? items_.takeAt(index) : nullptr;
}

void FlowLayout::setGeometry(const QRect& rect)
{
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}

QSize FlowLayout::minimumSize() const
{
    QSize size;
    for (const QLayoutItem* item : items_) size = size.expandedTo(item->minimumSize());
    const QMargins m = contentsMargins();
    return size + QSize(m.left() + m.right(), m.top() + m.bottom());
}

int FlowLayout::doLayout(const QRect& rect, bool testOnly) const
{
    const QMargins m = contentsMargins();
    const QRect area = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());

    // Lay out line by line so every item in a line can be vertically centered.
    int y = area.y();
    int i = 0;
    const int n = items_.size();
    while (i < n) {
        int lineWidth = 0, lineHeight = 0, j = i;
        while (j < n) {
            const QSize sz = items_[j]->sizeHint();
            const int extra = (j == i) ? sz.width() : hSpace_ + sz.width();
            if (j > i && lineWidth + extra > area.width()) break;
            lineWidth += extra;
            lineHeight = std::max(lineHeight, sz.height());
            ++j;
        }
        // Drop spacers at the start of a wrapped line
        int x = area.x();
        bool first = true;
        for (int k = i; k < j; ++k) {
            QLayoutItem* item = items_[k];
            const QSize sz = item->sizeHint();
            if (first && item->spacerItem() && k != 0) continue;
            if (!first) x += hSpace_;
            first = false;
            if (!testOnly) item->setGeometry(QRect(QPoint(x, y + (lineHeight - sz.height()) / 2), sz));
            x += sz.width();
        }
        y += lineHeight + vSpace_;
        i = j;
    }
    return y - vSpace_ - rect.y() + m.bottom();
}
