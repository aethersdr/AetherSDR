#pragma once

#include <QWidget>
#include <QPointer>

class QPushButton;
class QLabel;
class QLineEdit;
class QSlider;
class QComboBox;
class QStackedWidget;

namespace AetherSDR {

class HGauge;
class SliceModel;
class TransmitModel;

enum class MicMeterSessionState {
    Disconnected,
    Connected,
};

// P/CW applet — mode-aware panel that shows Phone controls (default) or CW
// controls when the active slice is in CW/CWL mode.  Both sub-panels live
// inside a QStackedWidget beneath a shared "P/CW" title bar.
class PhoneCwApplet : public QWidget {
    Q_OBJECT

public:
    // Narrow the mic-source dropdown to PC on a radio whose input this client
    // cannot choose (capability hasSelectableMicInputs). Idempotent.
    void setSelectableMicInputs(bool selectable);
    // Show or hide the mic-level gauge. False on a radio that defines no
    // microphone-peak meter at all — the face would be permanently at its floor
    // and read as a fault rather than as an absence.
    void setMicLevelMeterState(MicMeterSessionState session, bool available);
    void setDaxVisible(bool visible);
    void setCompressionMaximumDb(float maximum);
    void setCwControlLimits(int minWpm, int maxWpm, int minPitchHz,
                           int maxPitchHz, int pitchStepHz);
    void setSpeechProcessorPresentation(const QString& label, int maximum);
    explicit PhoneCwApplet(QWidget* parent = nullptr);

    void setTransmitModel(TransmitModel* model);

    // Bind the active slice.  TransmitModel is radio-global TX state; APF is a
    // per-slice receive filter, so the CW panel needs the slice itself to drive
    // its APF row (#4879).  Re-binding disconnects the previous slice first —
    // AppletPanel::setSlice is called on every active-slice change, so a
    // connect-only binding stacked a duplicate handler each time the operator
    // returned to a slice they had used before.
    void setSlice(SliceModel* slice);

    // Does the attached radio run a CW audio peaking filter in firmware?
    // Gates the APF row, whose only effect is `slice set <n> apf=` — a Flex
    // verb, not "any radio-side DSP". hasRadioSideDsp is too coarse: Icom
    // declares that true for NR/NB/notch and has no APF register. Pushed from
    // MainWindow::applyCapabilitiesToUi off RadioModel::hasAudioPeakingFilter(),
    // which is permissive while disconnected so the row does not blink off
    // on a Flex unplug.
    void setHasAudioPeakingFilter(bool has);

signals:
    void micLevelChanged(int level);  // slider value 0-100

    // Local CW sidetone — generated client-side by AudioEngine, independent
    // of the radio's DAX-fed sidetone.  MainWindow connects these to the
    // AudioEngine's CwSidetoneGenerator instance.
    // The single Sidetone toggle and volume slider drive both the radio's
    // DAX-fed sidetone and the local PortAudio-fed sidetone.  Pitch always
    // follows the radio's cw_pitch (no separate override).
    void sidetoneEnabledChanged(bool on);
    void sidetoneVolumeChanged(int pct);     // 0..100

public slots:
    // Phone meters (mic level / compression)
    void updateMeters(float micLevel, float compLevel,
                      float micPeak, float compPeak);
    void updateCompression(float compPeak);

    // Notify the applet when RADE mode activates/deactivates so the mic level
    // slider and meter behave correctly (client-side gain + RX metering).
    void setRadeActive(bool on);

    // CW meter (ALC 0–100)
    void updateAlc(float alc);
    void setAlcMeterUnit(const QString& unit);
    void resetAlc();

    // Switch between Phone and CW sub-panels based on slice mode.
    void setMode(const QString& mode);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void buildPhonePanel();
    void buildCwPanel();
    void syncPhoneFromModel();
    void syncCwFromModel();
    void syncApfFromSlice();
    void applyLevelMeterReceiveGate();
    void resetLevelMeter();

    TransmitModel* m_model{nullptr};
    // QPointer, not a raw pointer: MainWindow calls setSlice(nullptr) from
    // onSliceRemoved, by which point the outgoing SliceModel is already
    // deleteLater'd — disconnecting through a dangling raw pointer would be a
    // use-after-free on any path that drains the event loop first.
    QPointer<SliceModel> m_slice;
    QStackedWidget* m_stack{nullptr};
    QWidget* m_phonePanel{nullptr};
    QWidget* m_cwPanel{nullptr};

    // ── Phone sub-panel widgets ──────────────────────────────────────────

    HGauge* m_levelGauge{nullptr};
    MicMeterSessionState m_micLevelMeterSession{MicMeterSessionState::Disconnected};
    bool m_micLevelMeterAvailable{true};
    HGauge* m_compGauge{nullptr};

    QComboBox* m_micProfileCombo{nullptr};

    QComboBox*   m_micSourceCombo{nullptr};
    bool          m_selectableMicInputs{true};
    QSlider*     m_micLevelSlider{nullptr};
    QLabel*      m_micLevelLabel{nullptr};
    QPushButton* m_accBtn{nullptr};

    QPushButton* m_procBtn{nullptr};
    QSlider*     m_procSlider{nullptr};   // capability-shaped: presets or 0..100
    QLabel*      m_procLowLabel{nullptr};
    QLabel*      m_procMidLabel{nullptr};
    QLabel*      m_procHighLabel{nullptr};
    QPushButton* m_daxBtn{nullptr};

    QPushButton* m_monBtn{nullptr};
    QSlider*     m_monSlider{nullptr};
    QLabel*      m_monLabel{nullptr};

    // ── ALC gauges (mirrored across both Phone and CW panels) ───────────
    // Both gauges read from the same MeterModel::swAlcChanged source so
    // operators see the post-software-ALC SSB peak (dBFS) in whichever
    // panel is active for the current mode.
    HGauge*      m_alcGaugePhone{nullptr};
    HGauge*      m_alcGaugeCw{nullptr};

    // ── CW sub-panel widgets ─────────────────────────────────────────────

    QSlider*     m_delaySlider{nullptr};
    QLineEdit*   m_delayEdit{nullptr};

    QSlider*     m_speedSlider{nullptr};
    QLineEdit*   m_speedEdit{nullptr};

    QPushButton* m_sidetoneBtn{nullptr};
    QSlider*     m_sidetoneSlider{nullptr};
    QLineEdit*   m_sidetoneEdit{nullptr};

    QSlider*     m_cwPanSlider{nullptr};

    QPushButton* m_breakinBtn{nullptr};
    QPushButton* m_iambicBtn{nullptr};

    QString m_alcMeterUnit{QStringLiteral("dBFS")};
    float m_compressionMaximumDb{25.0f};
    int m_pitchMinHz{100};
    int m_pitchMaxHz{6000};
    int m_pitchStepHz{10};
    QLineEdit*   m_pitchEdit{nullptr};
    QPushButton* m_pitchDown{nullptr};
    QPushButton* m_pitchUp{nullptr};

    // APF — per-slice CW audio peaking filter, mirroring the VfoWidget DSP-tab
    // pair.  Both surfaces drive the same SliceModel, so they stay in sync
    // without any bridging between them (#4879).
    QWidget*     m_apfRow{nullptr};   // container, so the capability gate can hide the row whole
    QPushButton* m_apfBtn{nullptr};
    QSlider*     m_apfSlider{nullptr};
    QLineEdit*   m_apfEdit{nullptr};
    // Permissive default, matching RadioModel::hasAudioPeakingFilter()'s
    // disconnected rule: the row shows until a backend says otherwise, so it
    // does not blink out of existence on every Flex disconnect edge.
    bool m_hasAudioPeakingFilter{true};

    // ── Shared state ─────────────────────────────────────────────────────

    bool m_updatingFromModel{false};
    bool m_radeActive{false};

    // Client-side peak hold with slow decay for compression gauge
    float m_compHeld{0.0f};
    static constexpr float kCompDecayRate = 0.5f;
};

} // namespace AetherSDR
