// ╔════════════════════════════════════════════════════════════════════════════════╗
// ║   SweepScope.cpp                                                               ║
// ╚════════════════════════════════════════════════════════════════════════════════╝

#include "SweepScope.h"

#include "Compat.h"
#include "MeterProtocol.h"
#include "Theme.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr int kDivX = 10;
constexpr int kDivY = 10;
constexpr double kLeft = 46, kRight = 34, kTop = 26, kBottom = 22;
constexpr double kMinTop = -100, kMaxTop = 40;

const QColor kM1 = theme::Floor;
const QColor kM2 = theme::Peak;

QString timeText(double s)
{
    return QString::fromStdString(rfpm::formatTime(s));
}

}  // namespace

SweepScope::SweepScope(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumSize(320, 220);
    setFocusPolicy(Qt::ClickFocus);
}

const QVector<double>& SweepScope::scales()
{
    static const QVector<double> s{0.5, 1, 2, 3, 5, 10, 20};
    return s;
}

void SweepScope::setSweep(const QVector<float>& sweep)
{
    sweep_ = sweep;
    update();
}

void SweepScope::clearSweep()
{
    sweep_.clear();
    update();
}

void SweepScope::setTimebase(double windowSec, int samples, double periodSec)
{
    window_ = windowSec;
    samples_ = std::max(2, samples);
    period_ = periodSec;
    update();
}

int SweepScope::visibleSamples() const
{
    return std::min<int>(samples_, sweep_.size());
}

void SweepScope::setView(double dbPerDiv, double topDbm)
{
    scale_ = dbPerDiv > 0 ? dbPerDiv : 10;
    top_ = std::clamp(topDbm, kMinTop, kMaxTop);
    update();
}

void SweepScope::autoFit()
{
    const int n = visibleSamples();
    if (n == 0) return;
    const auto [mn, mx] = std::minmax_element(sweep_.begin(), sweep_.begin() + n);
    const double lo = *mn, hi = *mx;
    const double range = std::max(0.4, hi - lo);
    double scale = scales().back();
    for (double s : scales()) {
        if (range * 1.25 <= s * (kDivY - 2)) {
            scale = s;
            break;
        }
    }
    const double mid = (lo + hi) / 2;
    const double top = std::ceil((mid + scale * kDivY / 2.0) / scale) * scale;
    setView(scale, top);
    emit viewChanged(scale_, top_);
}

void SweepScope::setTriggerLevel(double dbm)
{
    trigger_ = dbm;
    update();
}

void SweepScope::setTriggerArmed(bool active)
{
    triggerArmed_ = active;
    update();
}

void SweepScope::setMarkers(double m1, double m2)
{
    m1_ = std::clamp(m1, 0.0, 1.0);
    m2_ = std::clamp(m2, 0.0, 1.0);
    update();
    emit markersChanged();
}

SweepScope::MarkerInfo SweepScope::markerInfo(int index) const
{
    const double f = index == 0 ? m1_ : m2_;
    MarkerInfo info{f * window_, std::numeric_limits<double>::quiet_NaN()};
    const int n = visibleSamples();
    if (n > 0) {
        const int i = std::clamp(int(std::floor(f * samples_)), 0, n - 1);
        info.dbm = sweep_[i];
    }
    return info;
}

void SweepScope::setBadge(const QString& text, const QColor& color)
{
    badge_ = text;
    badgeColor_ = color;
    update();
}

void SweepScope::setEmptyMessage(const QString& text)
{
    emptyMessage_ = text;
    update();
}

// ─── geometry ───────────────────────────────────────────────────────────────────

QRectF SweepScope::plotRect() const
{
    return QRectF(kLeft, kTop, std::max(10.0, width() - kLeft - kRight), std::max(10.0, height() - kTop - kBottom));
}

double SweepScope::yFor(double dbm, const QRectF& plot) const
{
    return plot.top() + (top_ - dbm) / (scale_ * kDivY) * plot.height();
}

double SweepScope::dbmForY(double y, const QRectF& plot) const
{
    return top_ - (y - plot.top()) / plot.height() * (scale_ * kDivY);
}

SweepScope::Drag SweepScope::hitTest(const QPointF& pos) const
{
    const QRectF plot = plotRect();
    const double ty = yFor(trigger_, plot);
    if (pos.x() > plot.right() && std::fabs(pos.y() - ty) <= 10) return Drag::Trigger;
    if (triggerArmed_ && plot.contains(pos) && std::fabs(pos.y() - ty) <= 4) return Drag::Trigger;
    const double x1 = plot.left() + m1_ * plot.width();
    const double x2 = plot.left() + m2_ * plot.width();
    if (pos.y() >= 0 && pos.y() <= plot.bottom()) {
        if (std::fabs(pos.x() - x1) <= 6) return Drag::Marker1;
        if (std::fabs(pos.x() - x2) <= 6) return Drag::Marker2;
    }
    return Drag::None;
}

// ─── mouse ──────────────────────────────────────────────────────────────────────

void SweepScope::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    const QPointF pos = eventPos(e);
    drag_ = hitTest(pos);
    if (drag_ == Drag::None) {
        const QRectF plot = plotRect();
        if (!plot.adjusted(0, -kTop, 0, 0).contains(pos)) return;
        // Click anywhere on the plot: the nearest marker jumps there
        const double x1 = plot.left() + m1_ * plot.width();
        const double x2 = plot.left() + m2_ * plot.width();
        drag_ = std::fabs(pos.x() - x1) <= std::fabs(pos.x() - x2) ? Drag::Marker1 : Drag::Marker2;
    }
    dragTo(pos);
}

void SweepScope::mouseMoveEvent(QMouseEvent* e)
{
    const QPointF pos = eventPos(e);
    hover_ = pos;
    if (drag_ != Drag::None) {
        dragTo(pos);
        return;
    }
    switch (hitTest(pos)) {
    case Drag::Trigger: setCursor(Qt::SizeVerCursor); break;
    case Drag::Marker1:
    case Drag::Marker2: setCursor(Qt::SizeHorCursor); break;
    default: setCursor(plotRect().contains(pos) ? Qt::CrossCursor : Qt::ArrowCursor); break;
    }
    update();
}

void SweepScope::mouseReleaseEvent(QMouseEvent*)
{
    drag_ = Drag::None;
}

void SweepScope::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) autoFit();
}

void SweepScope::leaveEvent(QEvent*)
{
    hover_ = QPointF(-1, -1);
    update();
}

void SweepScope::wheelEvent(QWheelEvent* e)
{
    const int steps = e->angleDelta().y() / 120;
    if (steps == 0) return;
    if (e->modifiers() & Qt::ControlModifier) {
        const auto& s = scales();
        int i = int(std::find(s.begin(), s.end(), scale_) - s.begin());
        if (i >= s.size()) i = s.indexOf(10.0);
        i = std::clamp(i - steps, 0, int(s.size()) - 1);   // wheel up = zoom in
        // keep the center of the screen where it is
        const double center = top_ - scale_ * kDivY / 2.0;
        const double scale = s[i];
        setView(scale, std::round((center + scale * kDivY / 2.0) / scale) * scale);
    } else {
        setView(scale_, top_ - steps * scale_);   // wheel up = trace moves up
    }
    emit viewChanged(scale_, top_);
    e->accept();
}

void SweepScope::dragTo(const QPointF& pos)
{
    const QRectF plot = plotRect();
    if (drag_ == Drag::Trigger) {
        double level = std::round(dbmForY(std::clamp(pos.y(), plot.top(), plot.bottom()), plot) * 2.0) / 2.0;
        level = std::clamp(level, -99.0, 30.0);
        if (level != trigger_) {
            trigger_ = level;
            emit triggerLevelChanged(trigger_);
            update();
        }
        return;
    }
    const double f = std::clamp((pos.x() - plot.left()) / plot.width(), 0.0, 1.0);
    if (drag_ == Drag::Marker1) m1_ = f;
    else if (drag_ == Drag::Marker2) m2_ = f;
    update();
    emit markersChanged();
}

// ─── paint ──────────────────────────────────────────────────────────────────────

void SweepScope::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(theme::Stroke, 1));
    p.setBrush(theme::ChartBg);
    p.drawRoundedRect(card, 14, 14);

    const QRectF plot = plotRect();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x0B, 0x07, 0x16));
    p.drawRoundedRect(plot.adjusted(-1, -1, 1, 1), 4, 4);

    // ── graticule ──
    const QPen divPen(theme::Grid, 1);
    const QPen axisPen(QColor(0x3A, 0x2C, 0x66), 1);
    for (int i = 0; i <= kDivX; ++i) {
        const double x = plot.left() + plot.width() * i / kDivX;
        p.setPen(i == kDivX / 2 ? axisPen : divPen);
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }
    for (int i = 0; i <= kDivY; ++i) {
        const double y = plot.top() + plot.height() * i / kDivY;
        p.setPen(i == kDivY / 2 ? axisPen : divPen);
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
    // minor ticks on the center axes, 5 per division
    p.setPen(axisPen);
    const double cx = plot.center().x(), cy = plot.center().y();
    for (int i = 0; i < kDivX * 5; ++i) {
        const double x = plot.left() + plot.width() * i / (kDivX * 5);
        p.drawLine(QPointF(x, cy - 3), QPointF(x, cy + 3));
    }
    for (int i = 0; i < kDivY * 5; ++i) {
        const double y = plot.top() + plot.height() * i / (kDivY * 5);
        p.drawLine(QPointF(cx - 3, y), QPointF(cx + 3, y));
    }

    // ── axis labels ──
    p.setFont(theme::uiFont(7.5));
    const QFontMetricsF fm(p.font());
    const int yEvery = plot.height() / kDivY < fm.height() * 1.4 ? 2 : 1;
    p.setPen(theme::TextFaint);
    for (int i = 0; i <= kDivY; i += yEvery) {
        const double y = plot.top() + plot.height() * i / kDivY;
        const double v = top_ - i * scale_;
        const QString label = QString::number(v, 'f', scale_ < 1 ? 1 : 0);
        p.drawText(QRectF(0, y - fm.height() / 2, kLeft - 7, fm.height()), Qt::AlignRight | Qt::AlignVCenter, label);
    }
    p.drawText(QRectF(4, 4, kLeft, kTop - 6), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("dBm"));
    const int xEvery = plot.width() / kDivX < fm.horizontalAdvance(QStringLiteral("000.0 ms")) ? 2 : 1;
    for (int i = 0; i <= kDivX; i += xEvery) {
        const double x = plot.left() + plot.width() * i / kDivX;
        const QString label = timeText(window_ * i / kDivX);
        Qt::Alignment al = Qt::AlignHCenter;
        QRectF box(x - 50, plot.bottom() + 3, 100, kBottom - 4);
        if (i == 0) { box.moveLeft(x); al = Qt::AlignLeft; }
        if (i == kDivX) { box.moveRight(x); al = Qt::AlignRight; }
        p.drawText(box, al | Qt::AlignVCenter, label);
    }

    // ── markers' span ──
    const double x1 = plot.left() + m1_ * plot.width();
    const double x2 = plot.left() + m2_ * plot.width();
    QColor span = theme::Accent;
    span.setAlpha(16);
    p.fillRect(QRectF(QPointF(std::min(x1, x2), plot.top()), QPointF(std::max(x1, x2), plot.bottom())), span);

    // ── trace ──
    const int n = visibleSamples();
    p.save();
    p.setClipRect(plot);
    if (n >= 2) {
        const double lo = top_ - scale_ * kDivY, hi = top_;
        QLinearGradient stroke(0, plot.top(), 0, plot.bottom());
        QLinearGradient fill(0, plot.top(), 0, plot.bottom());
        auto addStop = [&](double dbm, const QColor& c) {
            const double off = std::clamp((hi - dbm) / (hi - lo), 0.0, 1.0);
            stroke.setColorAt(off, c);
            QColor f = c;
            f.setAlpha(0x30);
            fill.setColorAt(off, f);
        };
        addStop(hi, theme::colorForDbm(hi));
        int ns = 0;
        const theme::Stop* stops = theme::strengthStops(&ns);
        for (int i = 0; i < ns; ++i) {
            if (stops[i].dbm > lo && stops[i].dbm < hi) addStop(stops[i].dbm, stops[i].color);
        }
        addStop(lo, theme::colorForDbm(lo));

        QPainterPath line;
        const double sx = plot.width() / samples_;
        for (int i = 0; i < n; ++i) {
            const double y = std::clamp(yFor(sweep_[i], plot), plot.top() - 4, plot.bottom() + 4);
            const QPointF pt(plot.left() + i * sx, y);
            if (i == 0) line.moveTo(pt);
            else line.lineTo(pt);
        }
        QPainterPath area = line;
        area.lineTo(plot.left() + (n - 1) * sx, plot.bottom());
        area.lineTo(plot.left(), plot.bottom());
        area.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawPath(area);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QBrush(stroke), n > 250 ? 1.4 : 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(line);
    } else {
        p.setPen(theme::TextFaint);
        p.setFont(theme::uiFont(11));
        p.drawText(plot, Qt::AlignCenter, emptyMessage_);
    }
    p.restore();

    // ── trigger level ──
    const double ty = yFor(trigger_, plot);
    if (ty >= plot.top() - 1 && ty <= plot.bottom() + 1) {
        QColor tc = theme::Warn;
        if (!triggerArmed_) tc.setAlpha(90);
        QPen tp(tc, 1.1);
        tp.setDashPattern({5, 4});
        p.setPen(tp);
        p.drawLine(QPointF(plot.left(), ty), QPointF(plot.right(), ty));
    }
    {
        // handle in the right margin (clamped so it stays reachable when off-screen)
        const double hy = std::clamp(ty, plot.top(), plot.bottom());
        QPainterPath flag;
        const double hx = plot.right() + 3;
        flag.moveTo(hx, hy);
        flag.lineTo(hx + 7, hy - 8);
        flag.lineTo(hx + 26, hy - 8);
        flag.lineTo(hx + 26, hy + 8);
        flag.lineTo(hx + 7, hy + 8);
        flag.closeSubpath();
        QColor fc = theme::Warn;
        if (!triggerArmed_) fc = QColor(0x6B, 0x4E, 0x16);
        p.setPen(Qt::NoPen);
        p.setBrush(fc);
        p.drawPath(flag);
        p.setPen(QColor(0x1A, 0x10, 0x05));
        p.setFont(theme::uiFont(7.5, QFont::Bold));
        p.drawText(QRectF(hx + 7, hy - 8, 19, 16), Qt::AlignCenter, QStringLiteral("T"));
    }

    // ── markers ──
    auto drawMarker = [&](double x, const QColor& c, const QString& tag, int index) {
        p.setPen(QPen(c, 1.2));
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        p.setFont(theme::uiFont(7.5, QFont::DemiBold));
        const QFontMetricsF tfm(p.font());
        const double tw = tfm.horizontalAdvance(tag) + 12;
        QRectF pill(x - tw / 2, 4, tw, kTop - 9);
        pill.moveLeft(std::clamp(pill.left(), 2.0, width() - tw - 2.0));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
        p.setPen(QColor(0x12, 0x0D, 0x1D));
        p.drawText(pill, Qt::AlignCenter, tag);
        const MarkerInfo info = markerInfo(index);
        if (!std::isnan(info.dbm)) {
            const double y = yFor(info.dbm, plot);
            if (y >= plot.top() && y <= plot.bottom()) {
                p.setPen(QPen(Qt::white, 1.2));
                p.setBrush(c);
                p.drawEllipse(QPointF(x, y), 3.5, 3.5);
            }
        }
    };
    drawMarker(x1, kM1, QStringLiteral("M1"), 0);
    drawMarker(x2, kM2, QStringLiteral("M2"), 1);

    // ── hover readout ──
    if (drag_ == Drag::None && plot.contains(hover_) && n > 0) {
        const double f = (hover_.x() - plot.left()) / plot.width();
        const int i = std::clamp(int(std::floor(f * samples_)), 0, n - 1);
        const QString text = QStringLiteral("%1   %2 dBm")
                                 .arg(timeText(f * window_))
                                 .arg(QString::fromStdString(rfpm::formatDbm(sweep_[i])));
        p.setFont(theme::monoFont(8.5));
        const QFontMetricsF hfm(p.font());
        QRectF box(0, 0, hfm.horizontalAdvance(text) + 14, hfm.height() + 6);
        box.moveBottomLeft(QPointF(hover_.x() + 12, plot.bottom() - 6));
        if (box.right() > plot.right()) box.moveRight(hover_.x() - 12);
        p.setPen(QPen(theme::StrokeBright, 1));
        p.setBrush(QColor(0x22, 0x18, 0x40, 230));
        p.drawRoundedRect(box, 6, 6);
        p.setPen(theme::Text);
        p.drawText(box, Qt::AlignCenter, text);
    }

    // ── badge ──
    if (!badge_.isEmpty()) {
        p.setFont(theme::uiFont(8, QFont::Bold));
        const QFontMetricsF bfm(p.font());
        QRectF b(0, 0, bfm.horizontalAdvance(badge_) + 16, bfm.height() + 6);
        b.moveTopRight(QPointF(plot.right() - 8, plot.top() + 8));
        QColor bg = badgeColor_;
        bg.setAlpha(40);
        p.setPen(QPen(badgeColor_, 1));
        p.setBrush(bg);
        p.drawRoundedRect(b, b.height() / 2, b.height() / 2);
        p.setPen(badgeColor_);
        p.drawText(b, Qt::AlignCenter, badge_);
    }
}
