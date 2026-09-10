#pragma once

#include <QWidget>
#include <QTimer>
#include <QElapsedTimer>
#include <QMetaObject>

class QPushButton;
class QLabel;
class QSlider;
class QComboBox;
class GuardedSlider;

namespace AetherSDR {

class AtuPreTuneDialog;
class BandPlanManager;
class RadioModel;
class TransmitModel;
class TunerModel;

// TX applet — transmit controls matching the SmartSDR TX panel.
//
// Layout (top to bottom):
//  - Title bar: "TX"
//  - Forward Power horizontal gauge (0–120 W, red > 100 W)
//  - SWR horizontal gauge (1.0–3.0, red > 2.5)
//  - RF Power slider (0–100%)
//  - Tune Power slider (0–100%)
//  - TX Profile dropdown + Success/Byp/Mem indicators
//  - TUNE / MOX / ATU / MEM buttons
//  - Active / Cal / Avail indicators
//  - APD button
class TxApplet : public QWidget {
    Q_OBJECT

public:
    explicit TxApplet(QWidget* parent = nullptr);

    void setTransmitModel(TransmitModel* model);
    void setTunerModel(TunerModel* tuner);

    // Wire the dialog dependencies for the ATU right-click menu — band plan
    // for sweep frequency derivation and the RadioModel for slice access
    // and command routing. (#2624)
    void setRadioModel(RadioModel* radio);
    void setBandPlanManager(BandPlanManager* bandPlan);

public slots:
    void updateMeters(float fwdPower, float swr, bool swrValid);
    // Capture raw pre-smoothed FWDPWR for PEP peak-hold tick. (#2561)
    void updatePeakPower(float fwdPowerInstant);
    // Reset the peak-hold tick when TX ends so a held peak does not linger
    // across overs. (#2561)
    void setTransmitting(bool tx);
    void setPowerScale(int maxWatts, bool hasAmplifier);

private:
    void buildUI();
    void syncFromModel();
    void syncAtuIndicators();
    // Single owner of the ATU/MEM enabled state.
    //
    // Two independent reasons exist to grey these out — the radio has no tuner
    // at all, and a TunerGenius XL is in Operate — and they arrive from
    // different models at different times. Before this they each called
    // setEnabled() directly, so whichever fired last won and a TGXL state change
    // could re-enable an ATU button on a radio that has no ATU.
    void updateAtuAvailability();
    // Right-click menu on the ATU button — exposes Pre-tune Bands and
    // Clear ATU Memories. Pre-tune is grayed when MEM is off. (#2624)
    void showAtuContextMenu(const QPoint& pos);
    void openPreTuneDialog();
    void confirmAndClearAtuMemories();
    // Right-click menu on the TUNE button — picks the carrier shape for
    // the *next* tune cycle: "Mono Tone" (single_tone) or "Two Tone".
    // Nothing is persisted — selecting Two Tone is a transient one-shot
    // and the radio reverts to single_tone on its own across power cycles.
    void showTuneContextMenu(const QPoint& pos);

    TransmitModel* m_model{nullptr};
    RadioModel*       m_radioModel{nullptr};
    BandPlanManager*  m_bandPlanMgr{nullptr};
    AtuPreTuneDialog* m_preTuneDialog{nullptr};

    // Frequency at which the ATU last reported a successful tune.
    // Used to gate the second-click → bypass behaviour: a click on the ATU
    // button only sends "atu bypass" when status is Successful/OK *and* the
    // current TX frequency still matches the freq we tuned at.  Any freq
    // change between clicks falls back to "atu start". (#1993)
    double m_atuTunedFreqMhz{-1.0};

    // Capability inputs to updateAtuAvailability(). Matching and Flex-style
    // memory operations are separate claims; both default permissive so an
    // applet built before a model reports retains the disconnected presentation.
    bool m_radioHasTuner{true};
    bool m_radioHasTunerMemories{true};
    bool m_tgxlOperate{false};

    // Gauges (HGauge*)
    QWidget* m_fwdGauge{nullptr};
    QWidget* m_swrGauge{nullptr};

    // Sliders
    GuardedSlider* m_rfPowerSlider{nullptr};
    GuardedSlider* m_tunePowerSlider{nullptr};
    QLabel*  m_rfPowerLabel{nullptr};
    QLabel*  m_tunePowerLabel{nullptr};

    // Profile dropdown
    QComboBox* m_profileCombo{nullptr};

    // ATU status indicators
    QLabel* m_successInd{nullptr};
    QLabel* m_bypInd{nullptr};
    QLabel* m_memInd{nullptr};
    QLabel* m_activeInd{nullptr};
    QLabel* m_calInd{nullptr};
    QLabel* m_availInd{nullptr};

    // Buttons
    QPushButton* m_tuneBtn{nullptr};
    QPushButton* m_moxBtn{nullptr};
    QPushButton* m_atuBtn{nullptr};
    QPushButton* m_memBtn{nullptr};
    QPushButton* m_apdBtn{nullptr};
    QWidget*     m_apdRow{nullptr};
public:
    // The APD row has TWO inputs and therefore ONE owner, for the same reason
    // the ATU button does: two callers each doing setVisible() means whichever
    // fires last wins.
    //
    //   apdConfigurable   the connected Flex reports `apd configurable=1`
    //   hasRadioSideDsp   the radio runs its own DSP at all
    //
    // Both are needed. apdConfigurable alone was already correct in practice on
    // an HL2 — nothing sets it — but only because the row's state carried over
    // from a previous session: it arrives ONLY in Flex TransmitDelta status, so a
    // backend that never sends it leaves the value to history, and m_apdRow is
    // constructed visible. The capability makes it deterministic.
    void setApdVisible(bool v)
    {
        m_apdConfigurable = v;
        updateApdVisibility();
    }
    void setRadioSideDspAvailable(bool v)
    {
        m_radioHasSideDsp = v;
        updateApdVisibility();
    }
private:
    void updateApdVisibility()
    {
        if (m_apdRow) {
            m_apdRow->setVisible(m_apdConfigurable && m_radioHasSideDsp);
        }
    }
    bool m_apdConfigurable{false};
    // Defaults TRUE so a widget built before any backend reports keeps its
    // pre-existing state; apdConfigurable's own false default is what actually
    // holds the row closed until a Flex says otherwise.
    bool m_radioHasSideDsp{true};

    bool m_updatingFromModel{false};
    // Presentation gate for TX-only meters. Meter replies can be in flight
    // across an un-key, so stopping the poller alone cannot prevent a late
    // non-zero sample from repainting an idle gauge.
    bool m_transmitting{false};
    bool m_forwardPowerRequiresSmoothing{true};
    bool m_forwardPowerScaleFollowsBandRating{false};
    QMetaObject::Connection m_capabilitiesConnection;

    // PEP peak-hold for the FWDPWR gauge — mirrors the SMeterWidget RX
    // peak-hold pattern.  The peak captures the highest pre-smoothed FWDPWR
    // sample, holds for ~2 s, then decays linearly toward the current
    // smoothed reading.  See HGauge::setPeakValue for the tick rendering and
    // SMeterWidget.cpp peak hold for the prior-art ballistics. (#2561)
    float m_smoothedPower{0.0f};
    float m_peakPower{0.0f};
    float m_peakDecayStart{0.0f};
    // Decay rate scaled to the gauge full-scale by setPowerScale so the
    // ~2.5 s visual feel stays consistent across rig classes.  Default
    // matches barefoot (120 W / 2.5 s) for the pre-connect case.
    float m_peakDecayWattsPerSec{48.0f};
    QElapsedTimer m_peakHoldTimer;
    bool m_peakHoldRunning{false};
    QTimer m_peakTick;

    // setPowerScale() no-ops when neither input moved (#4845) — it's called
    // on every RadioModel::infoChanged, most of which carry no scale-relevant
    // change, and gauge->setRange() forces a repaint.
    int  m_lastMaxWatts{-1};
    bool m_lastHasAmplifier{false};
    bool m_havePowerScale{false};
};

} // namespace AetherSDR
