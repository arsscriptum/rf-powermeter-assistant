// ╔════════════════════════════════════════════════════════════════════════════════╗
// ║   TimeSeriesChart.cpp                                                          ║
// ╚════════════════════════════════════════════════════════════════════════════════╝

#include "TimeSeriesChart.h"

#include "Compat.h"
#include "MeterProtocol.h"
#include "Theme.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {

/// Vertical gradient whose colors line up with the dBm axis (top = hi), so a stroked trace
/// is automatically colored by its value.
QLinearGradient axisGradient(double lo, double hi, double top, double bottom, int alpha)
{
    QLinearGradient g(0, top, 0, bottom);
    const double span = hi - lo;
    auto add = [&](double offset, QColor c) {
        c.setAlpha(alpha);
        g.setColorAt(std::clamp(offset, 0.0, 1.0), c);
    };
    add(0, theme::colorForDbm(hi));
    int n = 0;
    const theme::Stop* stops = theme::strengthStops(&n);
    for (int i = 0; i < n; ++i) {
        if (stops[i].dbm > lo && stops[i].dbm < hi) add((hi - stops[i].dbm) / span, stops[i].color);
    }
    add(1, theme::colorForDbm(lo));
    return g;
}

QString num(double v, int decimals = 1)
{
    return QString::fromStdString(rfpm::formatDbm(v, decimals));
}

}  // namespace

TimeSeriesChart::TimeSeriesChart(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumSize(240, 160);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void TimeSeriesChart::setData(const std::deque<ChartSample>* samples, double now, int windowSec, bool showMax,
                              double peakHoldDbm, double floorDbm)
{
    samples_ = samples;
    now_ = now;
    windowSec_ = std::max(1, windowSec);
    showMax_ = showMax;
    peak_ = peakHoldDbm;
    floor_ = floorDbm;
    update();
}

void TimeSeriesChart::setEmptyMessage(const QString& text)
{
    emptyMessage_ = text;
    update();
}

void TimeSeriesChart::mouseMoveEvent(QMouseEvent* e)
{
    hoverX_ = eventPos(e).x();
    update();
}

void TimeSeriesChart::leaveEvent(QEvent*)
{
    hoverX_ = -1;
    update();
}

void TimeSeriesChart::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Card
    const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(theme::Stroke, 1));
    p.setBrush(theme::ChartBg);
    p.drawRoundedRect(card, 14, 14);

    const QRectF plot = card.adjusted(10, 10, -10, -10);
    const double width = plot.width(), height = plot.height();
    if (width < 60 || height < 40) return;
    p.setClipRect(plot.adjusted(-2, -2, 2, 2));

    const double cutoff = now_ - windowSec_;
    const auto* bins = samples_;
    std::size_t first = bins ? bins->size() : 0;
    if (bins) {
        while (first > 0 && (*bins)[first - 1].t >= cutoff) --first;
    }
    const std::size_t count = bins ? bins->size() - first : 0;

    // Auto-range the Y axis to the visible data, snapped outward to 5 dB steps
    double dataMin = std::numeric_limits<double>::infinity();
    double dataMax = -std::numeric_limits<double>::infinity();
    for (std::size_t i = first; bins && i < bins->size(); ++i) {
        dataMin = std::min<double>(dataMin, (*bins)[i].min);
        dataMax = std::max<double>(dataMax, (*bins)[i].max);
    }
    if (count == 0) {
        dataMin = -90;
        dataMax = 0;
    }
    double lo = std::floor((dataMin - 2) / 5.0) * 5.0;
    double hi = std::ceil((dataMax + 2) / 5.0) * 5.0;
    if (hi - lo < 10) {
        const double c = (hi + lo) / 2;
        lo = c - 5;
        hi = c + 5;
    }
    lo = std::max(lo, -110.0);
    hi = std::min(hi, 30.0);
    const double span = hi - lo;
    auto Y = [&](double dbm) { return plot.top() + height - (dbm - lo) / span * height; };
    auto X = [&](double t) { return plot.left() + width - (now_ - t) / windowSec_ * width; };

    // Grid
    const QFont labelFont = theme::uiFont(7.5);
    p.setFont(labelFont);
    const QPen gridPen(theme::Grid, 1);
    double step = 20;
    for (double s : {1.0, 2.0, 5.0, 10.0, 20.0}) {
        if (span / s <= 9) {
            step = s;
            break;
        }
    }
    for (double v = std::ceil(lo / step) * step; v <= hi + 0.001; v += step) {
        const double y = Y(v);
        p.setPen(gridPen);
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        p.setPen(theme::TextFaint);
        p.drawText(QPointF(plot.left() + 3, y - 3), QString::number(v, 'f', 0));
    }
    for (int k = 1; k < 5; ++k) {
        const double back = windowSec_ * k / 5.0;
        const double x = plot.left() + width - back / windowSec_ * width;
        p.setPen(gridPen);
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        p.setPen(theme::TextFaint);
        const QString label = back >= 60 && std::fmod(back, 60.0) == 0 ? QStringLiteral("-%1 min").arg(back / 60)
                                                                      : QStringLiteral("-%1 s").arg(back, 0, 'f', 0);
        p.drawText(QPointF(x + 3, plot.bottom() - 4), label);
    }

    if (count == 0) {
        p.setPen(theme::TextFaint);
        p.setFont(theme::uiFont(11));
        p.drawText(plot, Qt::AlignCenter, emptyMessage_);
        return;
    }

    // Decimate to ~2 px columns: one avg + max per column regardless of history size
    constexpr double colW = 2.0;
    const int nCols = std::max(1, int(width / colW));
    colSum_.assign(nCols, 0.0);
    colCount_.assign(nCols, 0);
    colMax_.assign(nCols, -std::numeric_limits<double>::infinity());
    for (std::size_t i = first; i < bins->size(); ++i) {
        const ChartSample& s = (*bins)[i];
        const int c = int((X(s.t) - plot.left()) / colW);
        if (c < 0 || c >= nCols) continue;
        colSum_[c] += s.avg;
        colCount_[c]++;
        colMax_[c] = std::max<double>(colMax_[c], s.max);
    }
    auto colX = [&](int c) { return plot.left() + c * colW + colW / 2; };

    QPainterPath avgPath, maxPath, fillPath;
    double firstX = -1, lastX = -1;
    for (int c = 0; c < nCols; ++c) {
        if (colCount_[c] == 0) continue;
        const QPointF pt(colX(c), Y(colSum_[c] / colCount_[c]));
        const QPointF mx(colX(c), Y(colMax_[c]));
        if (firstX < 0) {
            firstX = pt.x();
            avgPath.moveTo(pt);
            maxPath.moveTo(mx);
            fillPath.moveTo(pt.x(), plot.bottom());
            fillPath.lineTo(pt);
        } else {
            avgPath.lineTo(pt);
            maxPath.lineTo(mx);
            fillPath.lineTo(pt);
        }
        lastX = pt.x();
    }
    if (firstX >= 0) {
        fillPath.lineTo(lastX, plot.bottom());
        fillPath.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(axisGradient(lo, hi, plot.top(), plot.bottom(), 0x3A));
        p.drawPath(fillPath);
        p.setBrush(Qt::NoBrush);
        if (showMax_) {
            p.setPen(QPen(QBrush(axisGradient(lo, hi, plot.top(), plot.bottom(), 0x90)), 1.1));
            p.drawPath(maxPath);
        }
        p.setPen(QPen(QBrush(axisGradient(lo, hi, plot.top(), plot.bottom(), 0xFF)), 2.0, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.drawPath(avgPath);
    }

    // Floor reference and peak hold
    if (!std::isnan(floor_) && floor_ >= lo && floor_ <= hi) {
        QPen pen(theme::Floor, 1.0, Qt::DotLine);
        p.setPen(pen);
        p.drawLine(QPointF(plot.left(), Y(floor_)), QPointF(plot.right(), Y(floor_)));
    }
    if (std::isfinite(peak_) && peak_ >= lo && peak_ <= hi) {
        QPen pen(theme::colorForDbm(peak_), 1.2);
        pen.setDashPattern({4, 4});
        p.setPen(pen);
        p.drawLine(QPointF(plot.left(), Y(peak_)), QPointF(plot.right(), Y(peak_)));
    }

    // Hover readout
    if (hoverX_ >= plot.left() && hoverX_ <= plot.right()) {
        const int c0 = int((hoverX_ - plot.left()) / colW);
        int best = -1;
        for (int d = 0; d < 12 && best < 0; ++d) {
            if (c0 - d >= 0 && c0 - d < nCols && colCount_[c0 - d]) best = c0 - d;
            else if (c0 + d < nCols && c0 + d >= 0 && colCount_[c0 + d]) best = c0 + d;
        }
        if (best >= 0) {
            const double x = colX(best);
            const double avg = colSum_[best] / colCount_[best];
            const double ago = (plot.left() + width - x) / width * windowSec_;
            p.setPen(QPen(theme::StrokeBright, 1));
            p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
            p.setPen(QPen(Qt::white, 1.5));
            p.setBrush(theme::colorForDbm(avg));
            p.drawEllipse(QPointF(x, Y(avg)), 4, 4);

            const QString text = QStringLiteral("-%1 s   %2 dBm   max %3")
                                     .arg(ago, 0, 'f', 1)
                                     .arg(num(avg, 1))
                                     .arg(num(colMax_[best], 1));
            p.setFont(theme::monoFont(9));
            const QFontMetrics fm(p.font());
            const QRectF box(0, 0, fm.horizontalAdvance(text) + 16, fm.height() + 8);
            QRectF placed = box.translated(x + 10, plot.top() + 8);
            if (placed.right() > plot.right()) placed.moveRight(x - 10);
            p.setPen(QPen(theme::StrokeBright, 1));
            p.setBrush(QColor(0x22, 0x18, 0x40, 235));
            p.drawRoundedRect(placed, 7, 7);
            p.setPen(theme::Text);
            p.drawText(placed, Qt::AlignCenter, text);
        }
    }
}
