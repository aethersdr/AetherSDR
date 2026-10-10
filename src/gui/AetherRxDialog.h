#pragma once

#include "ClientEqApplet.h"   // ClientEqApplet::Path
#include "CanonWindow.h"

#include <QVector>

#include <array>

class QAction;
class QHideEvent;
class QPushButton;
class QShowEvent;
class QStackedWidget;
class QTimer;

namespace AetherSDR {

class AudioEngine;
class AetherDspWidget;
class StageTabBar;
class StripCompPanel;
class StripEqPanel;
class StripGatePanel;
class StripPuduPanel;
class StripRxOutputPanel;
class StripTubePanel;
class StripWaveformPanel;
// AetherRX — the receive chain in one window, and the only RX surface. One tab
// per client-side RX stage down the left in signal order (NR, gate, EQ,
// compressor, tube, exciter, output meter), using the ModemChrome strip stood
// on end. The AetherNR tab holds AetherDspWidget whole, with its own method
// strip. Each stage tab has a bypass checkbox (the same flag as the RX chain
// strip's click-to-bypass); Out has none. The five chain-stage rows have drag
// grips that reorder AudioEngine's RX chain; AetherNR (runs ahead of the chain)
// and Out (the end meter) aren't in RxChainStage and stay first and last.
// A CanonWindow tool window (style guide, RFC #6226), like AetherTX.
class AetherRxDialog : public CanonWindow {
    Q_OBJECT

public:
    // The tabs, in the order they appear and in the order the signal meets
    // them. Used as a stack index, so entries are appended, never inserted.
    enum Stage { Nr = 0, Gate, Eq, Comp, Tube, Voice, Output, StageCount };

    explicit AetherRxDialog(AudioEngine* audio, QWidget* parent = nullptr);

    // Sync UI from current AudioEngine state.
    void syncFromEngine();

    // The profile library: save, load, import, export the receive chain.
    void showSettings();

    // The REC / PLAY pair, driven back by MainWindow with what the recorder
    // (or the radio) actually did -- the same three setters VfoWidget has,
    // so the wiring reads the same. Blocked, so a readback is never a click.
    void setRecordOn(bool on);
    void setPlayOn(bool on);
    void setPlayEnabled(bool enabled);

    // "TX Playback", the one entry on PLAY's context menu: transmit the last
    // recording over the active slice. The action is exposed so MainWindow
    // can register the bridge-guarded keying action on it (TxKeyingMarker);
    // selecting it again while a playback is transmitting stops it.
    QAction* txPlaybackAction() const { return m_txPlaybackAction; }
    void setTxPlaybackActive(bool on);

    // Jump to a named tab. Understands both this window's stage names
    // ("Gate", "Tube") and the noise-reduction method names ("NR2", "MNR"),
    // which select the AetherNR tab and then the method inside it — callers
    // that predate this window pass the latter.
    void selectTab(const QString& name);

    // The underlying tabbed widget — connect to its signals for parameter
    // change notifications.  Re-emitted on this dialog as well so existing
    // callers can keep connecting to the dialog directly.
    AetherDspWidget* widget() const { return m_widget; }

    // The receive EQ panel, for MainWindow to feed the slice's filter edges and
    // width ladder into -- the same push it already makes to the docked applet
    // and the floating editor.
    StripEqPanel* eqPanel() const { return m_eq; }

signals:
    // REC / PLAY clicked, with the state the button now shows. Same shape as
    // VfoWidget::recordToggled / playToggled, and MainWindow routes them the
    // same way: to the client-side QSO recorder or the active slice's
    // radio-side recorder, then calls the setters with what really happened.
    void recordToggled(bool on);
    void playToggled(bool on);
    // The operator chose "TX Playback" on PLAY's context menu. MainWindow
    // captures the transmit input at this boundary and keys, or stops the
    // playback already transmitting.
    void txPlaybackTriggered();

    // The AetherNR checkbox re-enabling NR2. Goes out rather than straight to
    // the engine because NR2 needs MainWindow's FFTW-wisdom prep first (#2275)
    // — the same reason AetherDspWidget and the chain strip both raise it.
    void nr2EnableWithWisdomRequested();

    // A receive filter width button was pressed on the EQ page.
    void rxFilterWidthRequested(int widthHz);

    // A filter edge was dragged on the EQ canvas. Same signal the docked applet
    // and the floating editor raise, so MainWindow applies it the same way:
    // audio-domain Hz, converted to slice offsets for the mode.
    void cutoffsDragRequested(ClientEqApplet::Path path,
                              int audioLowHz, int audioHighHz);

    // NR2 parameter changes (forwarded from m_widget)
    void nr2GainMaxChanged(float value);
    void nr2GainFloorChanged(float value);
    void nr2GainSmoothChanged(float value);
    void nr2QsppChanged(float value);
    void nr2GainMethodChanged(int method);
    void nr2NpeMethodChanged(int method);
    void nr2AeFilterChanged(bool on);
    void nr2Post2SettingsChanged();
    // MNR parameter changes
    void mnrStrengthChanged(float value);
    // DFNR parameter changes
    void rn2DryMixChanged(float mix);
    void dfnrAttenLimitChanged(float dB);
    void dfnrPostFilterBetaChanged(float beta);
    // NR4 parameter changes
    void nr4ReductionChanged(float dB);
    void nr4SmoothingChanged(float pct);
    void nr4WhiteningChanged(float pct);
    void nr4AdaptiveNoiseChanged(bool on);
    void nr4NoiseMethodChanged(int method);
    void nr4MaskingDepthChanged(float value);
    void nr4SuppressionChanged(float value);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QWidget* buildStagePage(QWidget* panel);
    void     addStage(Stage stage, const QString& label, QWidget* page);

    // Commit a checkbox to the engine, and read the engine back into one.
    void setStageEnabled(Stage stage, bool on);
    bool stageEnabled(Stage stage) const;


    AudioEngine*        m_audio{nullptr};
    AetherDspWidget*    m_widget{nullptr};
    StageTabBar*        m_tabs{nullptr};
    // The BYPASS toggle at the foot of the stage column, beside the Settings
    // gear, in the same spot AetherTX keeps its own. Owned by the StageTabBar; routes
    // through AudioEngine::setRxBypassed and follows rxBypassChanged.
    QPushButton*        m_bypassBtn{nullptr};
    // REC / PLAY on one row above BYPASS, as in AetherTX. These record the
    // receive audio itself, through the QSO recorder the VFO flag's pair
    // drives; the transmit window's pair captures the processed TX chain.
    QPushButton*        m_recBtn{nullptr};
    QPushButton*        m_playBtn{nullptr};
    QAction*            m_txPlaybackAction{nullptr};
    bool                m_txPlaybackActive{false};
    bool                m_playEnabled{false};   // what the host last said
    QStackedWidget*     m_stack{nullptr};
    QTimer*             m_checkTimer{nullptr};

    StripGatePanel*     m_gate{nullptr};
    StripEqPanel*       m_eq{nullptr};
    StripCompPanel*     m_comp{nullptr};
    StripTubePanel*     m_tube{nullptr};
    StripPuduPanel*     m_voice{nullptr};
    StripRxOutputPanel* m_output{nullptr};
    StripWaveformPanel* m_waveform{nullptr};
};

} // namespace AetherSDR
