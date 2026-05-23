#pragma once

#include <QWidget>
#include <QTimer>
#include <QElapsedTimer>

namespace MasterSDR {

// Analog S-Meter gauge widget matching the SmartSDR look.
//
// S-unit scale:
//   S0 = -127 dBm, S1 = -121 dBm, ... S9 = -73 dBm  (6 dB per S-unit)
//   S9+10 = -63 dBm, S9+20 = -53 dBm, S9+40 = -33 dBm, S9+60 = -13 dBm
//
// The needle sweeps from S0 (left) to S9+60 (right) across a 180° arc.
// Below S9 the scale markings are white; above S9 they are red.
class SMeterWidget : public QWidget {
    Q_OBJECT

public:
    explicit SMeterWidget(QWidget* parent = nullptr);

    QSize sizeHint() const override { return {280, 140}; }
    QSize minimumSizeHint() const override { return {200, 100}; }

    // Current reading in dBm.
    float levelDbm() const { return m_levelDbm; }

    // Reading as S-units string (e.g. "S7", "S9+20").
    QString sUnitsText() const;

    enum class TxMode { Power, SWR, Level, Compression };
    enum class RxMode { SMeter, SMeterPeak };
    enum class DecayRate { Fast, Medium, Slow };

public slots:
    // Update the displayed RX level (S-meter dBm).
    void setLevel(float dbm);

    // Update TX meter values.
    void setTxMeters(float fwdPower, float swr);

    // Update mic/compression meter values.
    void setMicMeters(float micLevel, float compLevel, float micPeak, float compPeak);

    // Switch between RX and TX needle source.
    void setTransmitting(bool tx);

    // Dropdown-driven mode selection.
    void setTxMode(const QString& mode);
    void setRxMode(const QString& mode);

    // Set TX power gauge scale: barefoot (120W), Aurora (600W), amplifier (2000W).
    void setPowerScale(int maxWatts, bool hasAmplifier);

    // Peak hold configuration.
    void setPeakHoldEnabled(bool enabled);
    void setPeakHoldTimeMs(int ms);
    void setPeakDecayRate(DecayRate rate);
    void setPeakDecayRate(const QString& rate);
    void resetPeak();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void updateNeedleTarget();
    void animateNeedle();
    void updatePeakHoldValue();

    // Map dBm to fraction (0.0 = left, 1.0 = right) for RX S-meter scale
    float dbmToFraction(float dbm) const;

    // Map TX value to fraction based on current TX mode
    float txValueToFraction(float value) const;

    // Get the current TX value based on mode
    float currentTxValue() const;

    // RX state
    float   m_levelDbm{-127.0f};    // current RX reading
    float   m_peakDbm{-127.0f};     // RX peak hold
    QString m_source{"S-Meter Peak"};

    // TX meter values (updated continuously, used when transmitting)
    float   m_txPower{0.0f};
    float   m_txSwr{1.0f};
    float   m_micLevel{-50.0f};
    float   m_compLevel{0.0f};

    // Mode state
    TxMode  m_txMode{TxMode::Power};
    RxMode  m_rxMode{RxMode::SMeter};
    bool    m_transmitting{false};

    QTimer  m_needleAnimation;
    QElapsedTimer m_needleElapsed;
    QTimer  m_peakDecay;
    QTimer  m_peakReset;    // hard reset peak hold every 10 seconds

    float   m_needleFraction{0.0f};
    float   m_targetNeedleFraction{0.0f};

    // Peak hold line state
    bool           m_peakHoldEnabled{false};
    float          m_peakHoldDbm{-127.0f};
    float          m_peakHoldDecayStartDbm{-127.0f};
    int            m_peakHoldTimeMs{1000};
    float          m_peakDecayDbPerSec{10.0f};  // Medium default
    QElapsedTimer  m_peakHoldTimer;
    bool           m_peakHoldTimerRunning{false};

    // S-unit reference: S0 = -127 dBm, each S-unit = 6 dB
    static constexpr float S0_DBM  = -127.0f;
    static constexpr float S9_DBM  = -73.0f;
    static constexpr float MAX_DBM = -13.0f;  // S9+60
    static constexpr float DB_PER_S = 6.0f;

    static constexpr int kNeedleAnimationIntervalMs = 8;
    static constexpr float kNeedleAttackTimeSeconds = 0.045f;
    static constexpr float kNeedleReleaseTimeSeconds = 0.180f;
    static constexpr float kNeedleSnapEpsilon = 0.001f;

    // TX Power gauge scale (dynamic)
    float m_powerScaleMax{120.0f};
    float m_powerRedStart{100.0f};

    // Arc geometry: shallow arc spanning ~70° (like SmartSDR)
    static constexpr float ARC_START_DEG = 55.0f;   // right end (degrees from +X axis)
    static constexpr float ARC_END_DEG   = 125.0f;  // left end
};

} // namespace MasterSDR
