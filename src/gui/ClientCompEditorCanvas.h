#pragma once

#include "ClientCompCurveWidget.h"

namespace MasterSDR {

// Interactive transfer curve for the floating editor.  Extends the
// read-only ClientCompCurveWidget with two draggable handles:
//
//   - Threshold handle: a triangle pointing right, sitting on the
//     input-axis bottom strip.  Dragging it left / right changes the
//     threshold.  Clamped to the displayed [-60, 0] dBFS range.
//
//   - Ratio handle: the filled dot at the knee centre.  Dragging it
//     vertically changes the ratio (upward = gentler, downward =
//     steeper).  Horizontal drag is ignored so the user can't change
//     threshold from the curve dot (that's the threshold handle's
//     job, keeping the gestures orthogonal).
//
// Emits atomic signals on every mouse-move so callers can live-update
// the DSP and settings.  Visual feedback comes from the parent's
// paintEvent, re-run after each mutation.
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

} // namespace MasterSDR
