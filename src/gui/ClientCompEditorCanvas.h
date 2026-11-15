#pragma once

#include "ClientCompCurveWidget.h"

namespace AetherSDR {

// Interactive transfer curve with two handles:
//   - Threshold: right-pointing triangle on the input-axis strip; horizontal
//     drag, clamped to [-60, 0] dBFS.
//   - Ratio: the knee dot; vertical drag only (up = gentler), so the gestures
//     stay orthogonal.
// Signals fire on every mouse move for live DSP/settings updates; the
// parent's paintEvent redraws.
class ClientCompEditorCanvas : public ClientCompCurveWidget {
    Q_OBJECT

public:
    explicit ClientCompEditorCanvas(QWidget* parent = nullptr);

signals:
    void thresholdChanged(float db);
    void ratioChanged(float ratio);

protected:
    void paintEvent(QPaintEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;
    // Per-region tooltips — QWidget::setToolTip is whole-widget; this
    // widget has two distinct draggable regions so we intercept the
    // tooltip event and dispatch based on the hover point.
    bool event(QEvent* ev) override;

private:
    enum class Drag { None, Threshold, Ratio };

    Drag   m_drag{Drag::None};
    QPointF m_dragStart;
    float   m_dragStartValue{0.0f};

    bool thresholdHandleHit(const QPointF& pos) const;
    bool ratioHandleHit(const QPointF& pos) const;
};

} // namespace AetherSDR
