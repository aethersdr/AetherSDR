#pragma once

// Selection frame for a canvas item (RFC #4887): border band with eight resize
// grips, drawn over the item. It covers the item's full rect but is masked to
// the band, so clicks inside pass through to the applet. No policy: presses are
// classified with hitZoneFor() and handed to the canvas session.

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
