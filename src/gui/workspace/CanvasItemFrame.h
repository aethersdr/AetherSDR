#pragma once

// The selection frame for a canvas item (RFC #4887 phase 5): a border band
// with eight resize grips, drawn OVER the selected item by the canvas.
//
// The one trick that makes this workable: the widget covers the item's full
// rect but is MASKED to the border band, so resize presses land on the frame
// while every click inside the band passes straight through to the applet —
// its sliders and buttons keep working with the frame up.  The containers
// themselves are untouched; selection chrome is entirely the canvas's.
//
// The frame owns no policy: it classifies presses with hitZoneFor() and
// hands the gesture to the canvas session, which does everything else.

#include "gui/workspace/CanvasInteraction.h"

#include <QWidget>

namespace AetherSDR {

class WorkspaceCanvas;

class CanvasItemFrame : public QWidget {
    Q_OBJECT

public:
    // Band width; also the grip square size.  8 px is wide enough to hit
    // without stealing meaningful content area.
    static constexpr int kGripPx = 8;

    explicit CanvasItemFrame(WorkspaceCanvas* canvas);

    // Cover `itemRectPx` (canvas coordinates) and re-mask to its border.
    void followItem(const QRect& itemRectPx);

protected:
    void paintEvent(QPaintEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;

private:
    HitZone zoneAt(const QPoint& localPos) const;

    WorkspaceCanvas* m_canvas{nullptr};
    bool m_dragging{false};
};

}  // namespace AetherSDR
