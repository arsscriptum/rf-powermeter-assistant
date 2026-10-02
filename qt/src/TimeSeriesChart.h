// ╔════════════════════════════════════════════════════════════════════════════════╗
// ║   TimeSeriesChart.h                                                            ║
// ║   Grafana-style rolling level chart ("signal finder")                          ║
// ╚════════════════════════════════════════════════════════════════════════════════╝
//
// Traces are colored by signal strength through a vertical gradient mapped to the dBm axis
// (violet floor -> blue -> amber -> orange -> red), with a soft area fill under the average.
// Data is decimated to 2 px columns, so the paint cost is bounded by the widget width, not
// by how much history has accumulated. Hovering shows the value under the cursor.

#pragma once

#include <QWidget>

#include <deque>
#include <limits>
#include <vector>

/// One chart point: the avg/min/max of the samples that arrived in one UI tick.
struct ChartSample
{
    double t;   // seconds, monotonic
    float avg;
    float min;
    float max;
};

class TimeSeriesChart : public QWidget
{
    Q_OBJECT

public:
    explicit TimeSeriesChart(QWidget* parent = nullptr);

    void setData(const std::deque<ChartSample>* samples, double now, int windowSec, bool showMax, double peakHoldDbm,
                 double floorDbm);
    void setEmptyMessage(const QString& text);

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent*) override;

private:
    const std::deque<ChartSample>* samples_ = nullptr;
    double now_ = 0;
    int windowSec_ = 60;
    bool showMax_ = true;
    double peak_ = -std::numeric_limits<double>::infinity();
    double floor_ = std::numeric_limits<double>::quiet_NaN();
    QString emptyMessage_;
    double hoverX_ = -1;

    // pooled decimation buffers
    std::vector<double> colSum_;
    std::vector<int> colCount_;
    std::vector<double> colMax_;
};
