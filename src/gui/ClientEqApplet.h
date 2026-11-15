#pragma once

#include <QWidget>

namespace AetherSDR {

class AudioEngine;
class ClientEqCurveWidget;

// Docked applet for client-side parametric EQ — single path, set at
// construction.  Shows a compact analyzer + summed EQ curve; the
// separate floating editor (opened from the chain widget via double-
// click) is where bands are added / removed / configured.
//
// One instance per side: AppletPanel constructs an Rx-bound copy
// for the RX chain tile and a Tx-bound copy for the TX chain tile.
// There's no internal Rx/Tx selector — the chain widget's tab is
// the single source of truth for which side the user is editing.
class ClientEqApplet : public QWidget {
    Q_OBJECT

public:
    enum class Path { Rx = 0, Tx = 1 };

    explicit ClientEqApplet(Path path, QWidget* parent = nullptr);

    void setAudioEngine(AudioEngine* engine);

    Path currentPath() const { return m_currentPath; }

    // Pull the Enable toggle state from the bound ClientEq — called by
    // MainWindow after the floating editor toggles its bypass button so
    // both views stay in sync.
    void refreshEnableFromEngine();

    // Push the radio's TX low/high filter cutoffs as dashed yellow guide
    // lines on the canvas.  No-op for RX-bound applets.
    void setTxFilterCutoffs(int lowHz, int highHz);

    // Push the active RX slice's filter passband (in audio-frequency
    // domain) as dashed yellow guide lines on the canvas.  No-op for
    // TX-bound applets.
    void setRxFilterCutoffs(int audioLowHz, int audioHighHz);

signals:
    // Fired when the user clicks Edit (currently no in-applet trigger;
    // editor opens via the chain widget's double-click gesture).
    // Retained for forward compat with future Edit affordance.
    void editRequested(Path path);

private:
    void buildUI();
    void syncEnableFromEngine();

    AudioEngine* m_audio{nullptr};
    const Path   m_currentPath{Path::Tx};

    ClientEqCurveWidget* m_curve{nullptr};
};

} // namespace AetherSDR
