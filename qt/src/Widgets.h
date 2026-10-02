// ╔════════════════════════════════════════════════════════════════════════════════╗
// ║   Widgets.h                                                                    ║
// ║   Small custom widgets: glowing readout, signal bar, status dot, flow layout   ║
// ╚════════════════════════════════════════════════════════════════════════════════╝

#pragma once

#include <QColor>
#include <QImage>
#include <QLayout>
#include <QVariantAnimation>
#include <QWidget>

#include <limits>

/// The big neon dBm value with a slow "breathing" glow behind it.
class GlowReadout : public QWidget
{
    Q_OBJECT

public:
    explicit GlowReadout(QWidget* parent = nullptr);
    void setText(const QString& text);
    QString text() const { return text_; }
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    void rebuildGlow();

    QString text_ = QStringLiteral("--.-");
    QFont font_;
    QImage glow_;
    bool glowDirty_ = true;
    double glowOpacity_ = 0.5;
    QVariantAnimation breathe_;
};

/// Horizontal level bar (gradient fill revealed up to the value) with a peak tick.
class SignalBar : public QWidget
{
    Q_OBJECT

public:
    explicit SignalBar(QWidget* parent = nullptr);
    void setRange(double lo, double hi);
    void setValue(double v);
    void setPeak(double v);
    QSize sizeHint() const override { return {200, 14}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    double lo_ = -80, hi_ = 10;
    double value_ = std::numeric_limits<double>::quiet_NaN();
    double peak_ = std::numeric_limits<double>::quiet_NaN();
};

/// Round connection-state indicator.
class StatusDot : public QWidget
{
public:
    explicit StatusDot(QWidget* parent = nullptr);
    void setColor(const QColor& c);
    QSize sizeHint() const override { return {12, 12}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QColor color_;
};

/// Wrapping row layout (the WPF WrapPanel): toolbars reflow instead of clipping.
class FlowLayout : public QLayout
{
public:
    explicit FlowLayout(QWidget* parent = nullptr, int hSpacing = 8, int vSpacing = 8);
    ~FlowLayout() override;

    void addItem(QLayoutItem* item) override;
    int count() const override { return items_.size(); }
    QLayoutItem* itemAt(int index) const override { return items_.value(index); }
    QLayoutItem* takeAt(int index) override;
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return doLayout(QRect(0, 0, width, 0), true); }
    void setGeometry(const QRect& rect) override;
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override;
    /// Insert a fixed gap (in px) before the next widget.
    void addSpacing(int px);

private:
    int doLayout(const QRect& rect, bool testOnly) const;

    QList<QLayoutItem*> items_;
    int hSpace_;
    int vSpace_;
};
