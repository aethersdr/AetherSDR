#pragma once

#include "ClientEqApplet.h"  // for Path enum
#include <QWidget>
#include <memory>

class QComboBox;
class QLabel;
class QPushButton;
class QTimer;

namespace MasterSDR {

class AudioEngine;
class ClientEqEditorCanvas;
class ClientEqFftAnalyzer;
class ClientEqIconRow;
class ClientEqOutputFader;
class ClientEqParamRow;

// Floating editor window for the client-side parametric EQ. One single
// instance lives on MainWindow; calling showForPath() swaps its bound
// ClientEq between RX and TX and raises the window. Geometry persists
// across shows via AppSettings (`StripEqPanelGeometry` key).
//
// Phase B.2 layout: title bar, path-indicator strip, large interactive
// curve canvas. Phases B.3+ add the filter-type icon row at the top,
// bottom parameter-text row with per-band columns, master gain knob on
// the right, and the FFT analyzer overlay inside the canvas.
class StripEqPanel : public QWidget {
    Q_OBJECT

public:
    explicit StripEqPanel(AudioEngine* engine, QWidget* parent = nullptr);
    ~StripEqPanel() override;

    // Switch the editor to the given path and show / raise the window.
    // Safe to call whether or not the window is currently visible.
    void showForPath(ClientEqApplet::Path path);

    // Push the radio's TX low/high filter cutoffs to the canvas as
    // dashed yellow guide lines.  No-op when the editor's current path
    // is RX.  Pass 0 for either edge to suppress that guide.
    void setTxFilterCutoffs(int lowHz, int highHz);

    // Push the active RX slice's filter passband (audio-frequency
    // domain) to the canvas.  No-op when the editor's current path is TX.
    // Cached so RX → TX → RX path swaps restore the correct guides.
    void setRxFilterCutoffs(int audioLowHz, int audioHighHz);

    // Reload every visual control (icon row, canvas, param row, family
    // combo, bypass button) from the engine's current band state.
    // Used after a preset import / load writes new values into the
    // engine without going through the canvas's own gestures, so the
    // text labels would otherwise stay stale.
    void refreshFromEngine();

signals:
    // Fired when the bypass button is toggled in the editor. The docked
    // applet subscribes so its Enable toggle stays in sync — both widgets
    // read/write the same ClientEq::enabled flag underneath.
    void bypassToggled(ClientEqApplet::Path path, bool bypassed);

    // Fired live during a cutoff-line drag.  Audio-domain Hz; MainWindow
    // converts back to TX-filter writes (path = Tx) or active-slice
    // filter writes with mode-aware offset reflection (path = Rx).
    void cutoffsDragRequested(ClientEqApplet::Path path,
                              int audioLowHz, int audioHighHz);

protected:
    void closeEvent(QCloseEvent* ev) override;
    void moveEvent(QMoveEvent* ev) override;
    void resizeEvent(QResizeEvent* ev) override;
    void showEvent(QShowEvent* ev) override;
    void hideEvent(QHideEvent* ev) override;

private:
    void saveGeometryToSettings();
    void restoreGeometryFromSettings();

    // Fans a band-selection change out to icon row, canvas, and param row
    // so all three views share the same highlighted column / handle.
    void syncSelection(int idx);

    // Refresh bypass-button state from the bound ClientEq's enabled flag.
    void syncBypassFromEq();

    // Pull latest post-EQ samples from AudioEngine, run the FFT, push
    // the smoothed bins into the curve widget. Called by m_fftTimer.
    void tickFftAnalyzer();

    AudioEngine*               m_audio{nullptr};
    ClientEqApplet::Path       m_path{ClientEqApplet::Path::Rx};
    int                        m_txFilterLowCutHz{0};
    int                        m_txFilterHighCutHz{0};
    int                        m_rxFilterLowCutHz{0};
    int                        m_rxFilterHighCutHz{0};
    int                        m_savedSmoothingFraction{96};
    // The frameless title bar carries the active path label (e.g.
    // "Masterial Parametric EQ — TX").  Held as a void* + cast at use
    // site to keep the inline EditorFramelessTitleBar class out of the
    // public header.
    QWidget*                   m_titleBar{nullptr};
    QComboBox*                 m_familyCombo{nullptr};
    QPushButton*               m_bypass{nullptr};
    ClientEqIconRow*           m_iconRow{nullptr};
    ClientEqEditorCanvas*      m_canvas{nullptr};
    ClientEqParamRow*          m_paramRow{nullptr};
    ClientEqOutputFader*       m_outFader{nullptr};
    QTimer*                    m_fftTimer{nullptr};
    std::unique_ptr<ClientEqFftAnalyzer> m_fftAnalyzer;
    bool                       m_restoring{false};
};

} // namespace MasterSDR
