#pragma once

#include "core/AudioEngine.h"

#include <QWidget>

class QLabel;
class QPushButton;

namespace MasterSDR {

class ClientChainWidget;
class ClientRxChainWidget;

// Docked chain tile — header with [TX] [RX] [BYPASS] buttons, the
// chain strip, and an interaction hint at the bottom.
//
// TX and RX buttons form an exclusive pair that selects which chain
// the widget displays.  TX is the working client-side DSP chain
// (six stages, all implemented).  RX is reserved for the future
// client-side RX DSP chain — until then, switching to RX shows a
// placeholder.
//
// BYPASS is a one-click action that disables every stage in the
// currently-selected chain.  Users re-enable individual stages via
// the chain widget's right-click menu or the per-stage applet tiles.
class ClientChainApplet : public QWidget {
    Q_OBJECT

public:
    enum class ChainMode { Tx, Rx };

    explicit ClientChainApplet(QWidget* parent = nullptr);

    void setAudioEngine(AudioEngine* engine);
    void refreshFromEngine();

    // Forwarded to the internal chain widget — see ClientChainWidget::
    // setMicInputReady.  MainWindow calls this whenever TransmitModel
    // reports a mic-state change (which covers both mic source and DAX
    // on/off).  Also drives the record button's enable state — no
    // audio to capture when the chain isn't in the signal path.
    void setMicInputReady(bool ready);

    // Forwarded — pulses the TX endpoint red when we're actively
    // transmitting on our own slice.  Driven by TransmitModel::
    // moxChanged from MainWindow.
    void setTxActive(bool active);

    // PUDU monitor state — MainWindow drives these from the monitor's
    // state signals.  Each toggles button appearance, pulse, and the
    // mutual-exclusion enable state of the other button.
    void setMonitorRecording(bool on);
    void setMonitorPlaying(bool on);
    void setMonitorHasRecording(bool has);

    // Phase 0 RX chassis — status-tile inputs forwarded to the RX
    // chain widget.  See ClientRxChainWidget for semantics.
    void setRxPcAudioEnabled(bool on);
    // label = active module's short name (e.g. "NR4"); empty → generic "DSP"
    void setRxClientDspActive(bool on, const QString& label = QString());
    void setRxOutputUnmuted(bool on);

    // Programmatic tab switch — used to restore PooDooAudioActiveTab
    // on startup.  Public so MainWindow can drive it after settings
    // load.  Does nothing if the requested mode is already active.
    void setActiveTab(ChainMode m);

signals:
    // Easter-egg launch nub at the bottom-right of the chain applet.
    // Wired in MainWindow to toggle the Masterial Audio Channel Strip
    // (unified TX DSP window).  See issue #2301.
    void MasterialStripToggleRequested();
    void stageEnabledChanged(AudioEngine::TxChainStage stage, bool enabled);
    // RX equivalents — forwarded from ClientRxChainWidget so MainWindow
    // can route them to the right editor / applet without depending on
    // the chain widget directly.
    void rxEditRequested(AudioEngine::RxChainStage stage);
    // Emitted when the user double-clicks the RX-chain DSP status tile.
    void rxDspEditRequested();
    // Forwarded from ClientRxChainWidget when single-click re-enables
    // NR2 from LastClientNr; MainWindow runs the wisdom-prep path.
    void rxNr2EnableWithWisdomRequested();
    void rxStageEnabledChanged(AudioEngine::RxChainStage stage, bool enabled);
    // Emitted after a successful drag-reorder of the RX chain.  Mirrors
    // chainReordered() above on the TX side.
    void rxChainReordered();
    // Emitted whenever the user flips the TX/RX tab.  MainWindow
    // routes this to AppletPanel::setPooDooActiveSide so the per-stage
    // tiles for the inactive side get hidden.
    void chainModeChanged(ChainMode mode);
    // Emitted after the user drags a stage to a new position.  MainWindow
    // subscribes so the applet-panel sub-container order can be kept
    // in lock-step with the chain order.
    void chainReordered();

    // User clicked the record or play button.  MainWindow decides
    // whether this is a start or a stop based on current monitor
    // state (click on a pulsing button = stop).
    void monitorRecordClicked();
    void monitorPlayClicked();

private:
    void setMode(ChainMode m);
    // Click handler for the BYPASS toggle.  On check: records which
    // TX stages are currently enabled, disables them all.  On uncheck:
    // re-enables just the stages that were on before.  Manual changes
    // between clicks are preserved only for stages not in the snapshot.
    void onBypassToggled(bool checked);

    // Applies the per-button stylesheet based on role + current state
    // (idle/active/pulse-bright/pulse-dim/disabled).  Separate method
    // so the pulse timer can call it cheaply at each tick.
    void applyRecordButtonStyle();
    void applyPlayButtonStyle();
    void updateMonitorButtonEnables();

    AudioEngine*         m_audio{nullptr};
    ClientChainWidget*   m_chain{nullptr};
    ClientRxChainWidget* m_rxChain{nullptr};
    QLabel*            m_hint{nullptr};
    QPushButton*       m_txBtn{nullptr};
    QPushButton*       m_rxBtn{nullptr};
    QPushButton*       m_bypassBtn{nullptr};
    QPushButton*       m_monRecBtn{nullptr};
    QPushButton*       m_monPlayBtn{nullptr};
    class QTimer*      m_monRecPulse{nullptr};
    class QTimer*      m_monPlayPulse{nullptr};
    bool               m_monRecPulseDim{false};
    bool               m_monPlayPulseDim{false};
    bool               m_monRecording{false};
    bool               m_monPlaying{false};
    bool               m_monHasRecording{false};
    bool               m_micReady{false};
    ChainMode          m_mode{ChainMode::Tx};
};

} // namespace MasterSDR
