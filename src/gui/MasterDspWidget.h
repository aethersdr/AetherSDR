#pragma once

#include <QWidget>
#include <array>

class QSlider;
class QLabel;
class QPushButton;
class QRadioButton;
class QCheckBox;
class QButtonGroup;
class QStackedWidget;

namespace MasterSDR {

class AudioEngine;

// MasterDsp settings body — the QTabWidget + per-tab controls + AppSettings
// persistence wiring shared by the modeless MasterDspDialog (Settings menu)
// and the docked ClientRxDspApplet (PooDoo Audio RX side).
//
// Signals fire on every parameter change (after the new value lands in
// AppSettings) so MainWindow can push the value into AudioEngine.  Both
// the dialog and the applet expose a `widget()` accessor so callers can
// connect to these signals directly.
class MasterDspWidget : public QWidget {
    Q_OBJECT

public:
    // DSP selector index — buttons in the selector row act as exclusive
    // activators for the six client-side noise-reduction modules.  The
    // button checked-state is the engine enable state; clicking the
    // active button toggles it off (no DSP active).
    enum DspId { NR2 = 0, NR4, MNR, DFNR, RN2, BNR, NumDsps };

    explicit MasterDspWidget(AudioEngine* audio, QWidget* parent = nullptr);

    // Sync UI from current AudioEngine + AppSettings state.
    void syncFromEngine();

    // Jump to a named tab (e.g. "MNR", "NR2", "DFNR").
    void selectTab(const QString& name);

    // Tighten padding / margins / fonts for the docked-applet variant.
    // The Settings-menu dialog leaves this off and renders at full size.
    void setCompactMode(bool on);

    // Scale every child QPushButton / QLabel font to 13 px to match the
    // VFO DSP toggle row.  The applet path leaves this off; only the
    // Settings-menu MasterDspDialog calls it.
    void setDialogMode(bool on);

    // Disable the NR2 selector button when compressed (Opus / SmartLink)
    // audio is active — NR2 amplifies codec artifacts (#1597).
    void setNr2Available(bool available, const QString& tooltip);

signals:
    // NR2 parameter changes
    void nr2GainMaxChanged(float value);
    void nr2GainSmoothChanged(float value);
    void nr2QsppChanged(float value);
    void nr2GainMethodChanged(int method);
    void nr2NpeMethodChanged(int method);
    void nr2AeFilterChanged(bool on);
    // MNR parameter changes
    void mnrEnabledChanged(bool on);
    void mnrStrengthChanged(float value);
    // DFNR parameter changes
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

    // Emitted instead of calling AudioEngine::setNr2Enabled(true) directly
    // so MainWindow can run the FFTW-wisdom prep first (PR #2275).  NR2
    // disable + every other DSP still go through the direct invokeMethod
    // path inside onDspButtonClicked.
    void nr2EnableWithWisdomRequested();

private:
    QWidget* buildNr2Page();
    QWidget* buildNr4Page();
    QWidget* buildMnrPage();
    QWidget* buildRn2Page();
    QWidget* buildBnrPage();
    QWidget* buildDfnrPage();

    // Restore defaults for the currently-selected DSP page.  No-op for
    // RN2 / BNR which expose no adjustable parameters.
    void resetCurrentTab();

    // Click handler for the per-DSP selector buttons.  index = DspId.
    void onDspButtonClicked(int index, bool nowChecked);

    // Push current AudioEngine enable state into the buttons + page stack
    // (without re-firing engine setters).  Called once at construction
    // and on every *EnabledChanged signal.
    void syncDspSelectorFromEngine();

    AudioEngine*    m_audio;
    QStackedWidget* m_dspStack{nullptr};
    std::array<QPushButton*, NumDsps> m_dspBtns{};

    // NR2 controls
    QButtonGroup* m_nr2GainGroup{nullptr};
    QButtonGroup* m_nr2NpeGroup{nullptr};
    QCheckBox*    m_nr2AeCheck{nullptr};
    QSlider*      m_nr2GainMaxSlider{nullptr};
    QLabel*       m_nr2GainMaxLabel{nullptr};
    QSlider*      m_nr2SmoothSlider{nullptr};
    QLabel*       m_nr2SmoothLabel{nullptr};
    QSlider*      m_nr2QsppSlider{nullptr};
    QLabel*       m_nr2QsppLabel{nullptr};

    // MNR controls
    QCheckBox*    m_mnrEnableCheck{nullptr};
    QSlider*      m_mnrStrengthSlider{nullptr};
    QLabel*       m_mnrStrengthLabel{nullptr};

    // NR4 controls
    QSlider*      m_nr4ReductionSlider{nullptr};
    QLabel*       m_nr4ReductionLabel{nullptr};
    QSlider*      m_nr4SmoothingSlider{nullptr};
    QLabel*       m_nr4SmoothingLabel{nullptr};
    QSlider*      m_nr4WhiteningSlider{nullptr};
    QLabel*       m_nr4WhiteningLabel{nullptr};
    QCheckBox*    m_nr4AdaptiveCheck{nullptr};
    QButtonGroup* m_nr4MethodGroup{nullptr};
    QSlider*      m_nr4MaskingSlider{nullptr};
    QLabel*       m_nr4MaskingLabel{nullptr};
    QSlider*      m_nr4SuppressionSlider{nullptr};
    QLabel*       m_nr4SuppressionLabel{nullptr};

    // DFNR controls
    QSlider*      m_dfnrAttenSlider{nullptr};
    QLabel*       m_dfnrAttenLabel{nullptr};
    QSlider*      m_dfnrBetaSlider{nullptr};
    QLabel*       m_dfnrBetaLabel{nullptr};
};

} // namespace MasterSDR
