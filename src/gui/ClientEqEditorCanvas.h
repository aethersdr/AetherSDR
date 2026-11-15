#pragma once

#include "ClientEqCurveWidget.h"

namespace AetherSDR {

class AudioEngine;

// Interactive curve for the floating editor. L-drag a handle: freq + gain;
// Shift+L-drag: Q. Double-click empty area: new band, type chosen by position
// (HP left edge, LP right edge, shelves near top/bottom, else peak).
// Right-click a handle: cycle type / toggle enable / delete. Every mutation
// writes to ClientEq and calls AudioEngine::saveClientEqSettings().
class ClientEqEditorCanvas : public ClientEqCurveWidget {
    Q_OBJECT

public:
    explicit ClientEqEditorCanvas(QWidget* parent = nullptr);

    // Audio-engine pointer is needed for persistence callbacks after each
    // edit.  The ClientEq pointer itself is set via setEq() on the base.
    void setAudioEngine(AudioEngine* engine);

signals:
    // Emitted live during a cutoff-line drag.  Audio-domain Hz values;
    // the editor / MainWindow translate to TX-filter or RX-slice writes.
    void cutoffsDragged(int audioLowHz, int audioHighHz);

protected:
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;
    void mouseDoubleClickEvent(QMouseEvent* ev) override;
    void contextMenuEvent(QContextMenuEvent* ev) override;

private:
    // Hit-test pixel point against all active handles.  Returns band index
    // or -1 if no handle within kHandleHitRadius.
    int hitTestHandle(const QPointF& pos) const;

    enum class CutoffEdge { None, Low, High };
    // Hit-test against the dashed yellow cutoff guide lines.  Returns
    // which edge (if any) the cursor is within ~5 px of horizontally.
    // Excludes the band-plan strip area at the bottom.
    CutoffEdge hitTestCutoffEdge(const QPointF& pos) const;

    // Save current band state to settings (called after every edit so
    // the user doesn't lose work on crash / quit).
    void persist();

    AudioEngine* m_audio{nullptr};
    int  m_draggingBand{-1};
    bool m_dragShift{false};
    QPointF m_dragStart;
    float   m_dragStartFreqHz{0};
    float   m_dragStartGainDb{0};
    float   m_dragStartQ{0};

    CutoffEdge m_draggingCutoff{CutoffEdge::None};
};

} // namespace AetherSDR
