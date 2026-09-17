#pragma once

#include "PersistentDialog.h"

class QButtonGroup;
class QFrame;
class QStackedWidget;

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

signals:
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

private:
    QWidget* buildStagePage(QWidget* panel);
    void     addStage(Stage stage, const QString& label, QWidget* page);

    AetherDspWidget*    m_widget{nullptr};
    QFrame*             m_tabsFrame{nullptr};
    QButtonGroup*       m_tabGroup{nullptr};
    QStackedWidget*     m_stack{nullptr};

    StripGatePanel*     m_gate{nullptr};
    StripEqPanel*       m_eq{nullptr};
    StripCompPanel*     m_comp{nullptr};
    StripTubePanel*     m_tube{nullptr};
    StripPuduPanel*     m_voice{nullptr};
    StripRxOutputPanel* m_output{nullptr};
    StripWaveformPanel* m_waveform{nullptr};
};

} // namespace AetherSDR
