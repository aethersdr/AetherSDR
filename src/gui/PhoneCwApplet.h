#pragma once

#include <QWidget>

class QPushButton;
class QLabel;
class QLineEdit;
class QSlider;
class QComboBox;
class QStackedWidget;

namespace MasterSDR {

class HGauge;
class TransmitModel;

// P/CW applet — mode-aware panel that shows Phone controls (default) or CW
// controls when the active slice is in CW/CWL mode.  Both sub-panels live
// inside a QStackedWidget beneath a shared "P/CW" title bar.
class PhoneCwApplet : public QWidget {
    Q_OBJECT

public:
    explicit PhoneCwApplet(QWidget* parent = nullptr);

    void setTransmitModel(TransmitModel* model);

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

    // Switch between Phone and CW sub-panels based on slice mode.
    void setMode(const QString& mode);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void buildPhonePanel();
    void buildCwPanel();
    void syncPhoneFromModel();
    void syncCwFromModel();

    TransmitModel* m_model{nullptr};
    QStackedWidget* m_stack{nullptr};
    QWidget* m_phonePanel{nullptr};
    QWidget* m_cwPanel{nullptr};

    // ── Phone sub-panel widgets ──────────────────────────────────────────

    HGauge* m_levelGauge{nullptr};
    HGauge* m_compGauge{nullptr};

    QComboBox* m_micProfileCombo{nullptr};

    QComboBox*   m_micSourceCombo{nullptr};
    QSlider*     m_micLevelSlider{nullptr};
    QLabel*      m_micLevelLabel{nullptr};
    QPushButton* m_accBtn{nullptr};

    QPushButton* m_procBtn{nullptr};
    QSlider*     m_procSlider{nullptr};   // 3-position: 0=NOR, 1=DX, 2=DX+
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

    QLineEdit*   m_pitchEdit{nullptr};
    QPushButton* m_pitchDown{nullptr};
    QPushButton* m_pitchUp{nullptr};

    // ── Shared state ─────────────────────────────────────────────────────

    bool m_updatingFromModel{false};
    bool m_radeActive{false};

    // Client-side peak hold with slow decay for compression gauge
    float m_compHeld{0.0f};
    static constexpr float kCompDecayRate = 0.5f;
};

} // namespace MasterSDR
