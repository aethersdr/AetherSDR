#pragma once

#include <QWidget>
#include <QTimer>
#include <QElapsedTimer>
#include <QPixmap>

#include "AnalogMeterFaceTheme.h"
#include "core/KiwiSdrProtocol.h"
#include "RadioSwrValidityFilter.h"
#include "SMeterGeometry.h"

class QPainter;

namespace AetherSDR {

// Analog S-Meter gauge widget matching the SmartSDR look.
//
// S-unit scale:
//   S0 = -127 dBm, S1 = -121 dBm, ... S9 = -73 dBm  (6 dB per S-unit)
//   S9+10 = -63 dBm, S9+20 = -53 dBm, S9+40 = -33 dBm, S9+60 = -13 dBm
//
// The needle sweeps from S0 (left) to S9+60 (right) across a shallow 70° arc.
// Below S9 the scale markings are white; above S9 they are red.
class SMeterWidget : public QWidget {
    Q_OBJECT

public:
    explicit SMeterWidget(QWidget* parent = nullptr);

    QSize sizeHint() const override { return m_geometry.sizing.preferred; }
    QSize minimumSizeHint() const override { return m_geometry.sizing.minimum; }

    // Current reading in dBm.
    float levelDbm() const { return m_levelDbm; }

    // Reading as S-units string (e.g. "S7", "S9+20").
    QString sUnitsText() const;

    const SMeterGeometry& geometry() const { return m_geometry; }
    AnalogMeterFaceTheme faceTheme() const { return m_faceTheme; }
    QString faceThemeId() const;
    QString accessibleValueText() const;

    // Let a detached meter consume its resizable window while preserving the
    // compact fixed-height sidebar layout when docked.
    void setFloating(bool floating);
    void setFaceTheme(AnalogMeterFaceTheme theme);

    enum class TxMode { Power, SWR, Level, Compression };
    enum class RxMode { SMeter, SMeterPeak };
    enum class DecayRate { Fast, Medium, Slow };

public slots:
    // Update the displayed RX level (S-meter dBm).
    void setLevel(float dbm);
    void setReceiveMeterReading(
        const AetherSDR::KiwiSdrProtocol::MeterReading& reading);

    // Update TX meter values.
    void setTxMeters(float fwdPower, float swr);
    // The radio-native SWR value remains the displayed source. Instantaneous
    // forward power is used only to reject samples taken without measurable RF.
    void setRadioTxMeters(float fwdPower, float fwdPowerInstant, float swr);

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
    bool usesUnavailableRxMeter() const;
    QString unavailableRxMeterLabel() const;
    QString sUnitsTextFor(float dbm) const;
    void scheduleAccessibleValue();
    void publishAccessibleValue();
    void rebuildBackgroundLayer();
    void drawPhysicalNeedle(QPainter& painter, const QPointF& pivot,
                            const QPointF& tip,
                            const QSizeF& faceSize,
                            const AnalogMeterFaceThemeCatalog::Palette& palette) const;
    void drawPhysicalMask(QPainter& painter, const QRectF& face,
                          const AnalogMeterFaceThemeCatalog::Palette& palette) const;
    void finishTxMeterUpdate();
    void updateRadioSwr(float forwardPowerInstant, float swr, qint64 timestampMs);

    // Map dBm to fraction (0.0 = left, 1.0 = right) for RX S-meter scale
    float dbmToFraction(float dbm) const;

    // Map TX value to fraction based on current TX mode
    float txValueToFraction(float value) const;

    // Get the current TX value based on mode
    float currentTxValue() const;

    // RX state
    SMeterGeometry m_geometry;
    AnalogMeterFaceThemeCatalog m_faceThemes;
    AnalogMeterFaceTheme m_faceTheme{AnalogMeterFaceTheme::AetherDefault};
    QPixmap m_backgroundLayer;
    QSize m_backgroundCacheSize;
    qreal m_backgroundCacheDpr{0.0};
    AnalogMeterFaceTheme m_backgroundCacheTheme{AnalogMeterFaceTheme::AetherDefault};
    bool m_backgroundCacheValid{false};
    QTimer m_accessibilityTimer;
    QString m_lastAccessibleValue;
    float   m_levelDbm{0.0f};    // current RX reading; initialized from geometry
    float   m_peakDbm{0.0f};     // RX peak hold; initialized from geometry
    QString m_source{"S-Meter Peak"};
    KiwiSdrProtocol::MeterReading m_receiveMeterReading;
    bool m_receiveMeterReadingActive{false};

    // TX meter values (updated continuously, used when transmitting)
    float   m_txPower{0.0f};
    float   m_txSwr{1.0f};
    RadioSwrValidityFilter m_radioSwrFilter;
    QElapsedTimer m_radioSwrClock;
    float   m_micLevel{-50.0f};  // MIC meter — drives Level mode needle
    float   m_micPeak{-50.0f};   // MICPEAK meter — reserved for future peak tick
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
    float          m_peakHoldDbm{0.0f};
    float          m_peakHoldDecayStartDbm{0.0f};
    int            m_peakHoldTimeMs{1000};
    float          m_peakDecayDbPerSec{10.0f};  // Medium default
    QElapsedTimer  m_peakHoldTimer;
    bool           m_peakHoldTimerRunning{false};

    static constexpr int kNeedleAnimationIntervalMs = 8;
    static constexpr int kAccessibilityAnnouncementIntervalMs = 100;
    static constexpr float kNeedleAttackTimeSeconds = 0.045f;
    static constexpr float kNeedleReleaseTimeSeconds = 0.180f;
    static constexpr float kNeedleSnapEpsilon = 0.001f;

    // TX Power gauge scale (dynamic)
    float m_powerScaleMax{120.0f};
    float m_powerRedStart{100.0f};

};

} // namespace AetherSDR
