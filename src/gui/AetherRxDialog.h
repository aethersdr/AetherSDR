#pragma once

#include "ClientEqApplet.h"   // ClientEqApplet::Path
#include "PersistentDialog.h"

#include <array>

class QButtonGroup;
class QCheckBox;
class QHideEvent;
class QShowEvent;
class QFrame;
class QStackedWidget;
class QTimer;

namespace AetherSDR {

class AudioEngine;
class AetherDspWidget;
class StripCompPanel;
class StripEqPanel;
class StripGatePanel;
class StripPuduPanel;
class StripRxOutputPanel;
class StripTubePanel;
class StripWaveformPanel;
// AetherRX — the receive chain in one window.
//
// Every client-side RX stage lives here, one per tab down the left-hand side
// in signal order: noise reduction, gate, EQ, compressor, tube, voice
// processor, and the output meter with its waveform. The tabs are the same
// ModemChrome strip the AetherDSP body uses along its top, stood on end.
//
// This is the only RX surface: the Aetherial strip is AetherTX now and no
// longer carries a receive page, so these panel instances are not competing
// with a second copy.
//
// The AetherNR tab holds AetherDspWidget whole, including its own horizontal
// method strip — the seven noise-reduction methods stay one click apart
// rather than becoming seven more entries in this bar.
//
// Each tab carries a checkbox that enables or bypasses that stage, the same
// flag the RX chain strip's click-to-bypass toggles — this window is where you
// set a stage up, so it is where you should be able to switch it off. Out is
// the exception: a meter and a waveform are not a stage and have nothing to
// bypass, so that row is indented to the others' labels and carries no box.
class AetherRxDialog : public PersistentDialog {
    Q_OBJECT

public:
    // The tabs, in the order they appear and in the order the signal meets
    // them. Used as a stack index, so entries are appended, never inserted.
    enum Stage { Nr = 0, Gate, Eq, Comp, Tube, Voice, Output, StageCount };

    explicit AetherRxDialog(AudioEngine* audio, QWidget* parent = nullptr);

    // Sync UI from current AudioEngine state.
    void syncFromEngine();

    // Jump to a named tab. Understands both this window's stage names
    // ("AGC-G", "Tube") and the noise-reduction method names ("NR2", "MNR"),
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
    // Pull every box from the engine. The Client* stages are plain classes
    // with no change signal, and the chain strip can toggle the same flags
    // behind this window's back, so the boxes are polled while it is visible.
    void refreshStageChecks();

    AudioEngine*        m_audio{nullptr};
    AetherDspWidget*    m_widget{nullptr};
    QFrame*             m_tabsFrame{nullptr};
    QButtonGroup*       m_tabGroup{nullptr};
    QStackedWidget*     m_stack{nullptr};
    std::array<QCheckBox*, StageCount> m_stageChecks{};
    QWidget*            m_outIndent{nullptr};
    QTimer*             m_checkTimer{nullptr};
    // Set while refreshStageChecks() is writing, so a box being brought in
    // line with the engine does not turn round and write back to it.
    bool                m_syncingChecks{false};

    StripGatePanel*     m_gate{nullptr};
    StripEqPanel*       m_eq{nullptr};
    StripCompPanel*     m_comp{nullptr};
    StripTubePanel*     m_tube{nullptr};
    StripPuduPanel*     m_voice{nullptr};
    StripRxOutputPanel* m_output{nullptr};
    StripWaveformPanel* m_waveform{nullptr};
};

} // namespace AetherSDR
