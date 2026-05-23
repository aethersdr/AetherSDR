#pragma once

#include "ClientEqCurveWidget.h"

namespace MasterSDR {

class AudioEngine;

// Interactive version of the curve widget used inside the floating editor.
// Adds mouse handling on top of ClientEqCurveWidget's rendering:
//
//   - L-drag on a band handle: freq + gain
//   - Shift + L-drag on a handle: Q (vertical axis maps to Q)
//   - Double-click empty area: create a new band; filter type is chosen
//     by position (HP at left edge, LP at right edge, shelves near top/
//     bottom extremes, peak everywhere else)
//   - Right-click on a handle: context menu (cycle type, toggle enable,
//     delete)
//
// Each mutation writes through to the ClientEq instance and calls
// AudioEngine::saveClientEqSettings() so the change persists across
// restarts.
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

} // namespace MasterSDR
