// ╔════════════════════════════════════════════════════════════════════════════════╗
// ║   SweepScope.h                                                                 ║
// ║   Oscilloscope view of one 500-sample sweep (the vendor app's main screen)     ║
// ╚════════════════════════════════════════════════════════════════════════════════╝
//
// Interaction:
//   drag on the plot          move the nearest marker (M1 / M2)
//   drag the T handle / line  set the trigger level
//   wheel                     move the view up / down by one division
//   Ctrl + wheel              change the dB/div scale
//   double-click              auto-fit the view to the current sweep

#pragma once

#include <QColor>
#include <QVector>
#include <QWidget>

class SweepScope : public QWidget
{
    Q_OBJECT

public:
    struct MarkerInfo
    {
        double time;   // seconds from the start of the sweep
        double dbm;    // NaN when there is no sweep
    };

    explicit SweepScope(QWidget* parent = nullptr);

    /// The dB/div steps offered by the scale control.
    static const QVector<double>& scales();

    void setSweep(const QVector<float>& sweep);
    void clearSweep();
    const QVector<float>& sweep() const { return sweep_; }
    bool hasSweep() const { return !sweep_.isEmpty(); }

    void setTimebase(double windowSec, int samples, double periodSec);
    double windowSec() const { return window_; }
    int visibleSamples() const;
    double periodSec() const { return period_; }

    void setView(double dbPerDiv, double topDbm);
    double scale() const { return scale_; }
    double top() const { return top_; }
    void autoFit();

    void setTriggerLevel(double dbm);
    double triggerLevel() const { return trigger_; }
    void setTriggerArmed(bool active);   // active (Normal / Single) = bright dashed line

    void setMarkers(double m1, double m2);
    double markerFraction(int index) const { return index == 0 ? m1_ : m2_; }
    MarkerInfo markerInfo(int index) const;

    void setBadge(const QString& text, const QColor& color);
    void setEmptyMessage(const QString& text);

signals:
    void markersChanged();
    void triggerLevelChanged(double dbm);
    void viewChanged(double dbPerDiv, double topDbm);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void leaveEvent(QEvent*) override;

private:
    enum class Drag { None, Marker1, Marker2, Trigger };

    QRectF plotRect() const;
    double yFor(double dbm, const QRectF& plot) const;
    double dbmForY(double y, const QRectF& plot) const;
    Drag hitTest(const QPointF& pos) const;
    void dragTo(const QPointF& pos);

    QVector<float> sweep_;
    double window_ = 1e-3;
    int samples_ = 500;
    double period_ = 2e-6;
    double scale_ = 10.0;
    double top_ = 10.0;
    double trigger_ = -40.0;
    bool triggerArmed_ = false;
    double m1_ = 0.15;
    double m2_ = 0.55;
    QString badge_;
    QColor badgeColor_;
    QString emptyMessage_;

    Drag drag_ = Drag::None;
    QPointF hover_{-1, -1};
};
