#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;
class QHideEvent;
class QMouseEvent;
class QShowEvent;
class QTimer;

namespace AetherSDR {

class MeterModel;
class HealthGraphWidget;

struct AntennaHealthSample {
    float powerWatts{0.0f};
    float swr{1.0f};
    float powerAverage{0.0f};
    float swrAverage{1.0f};
    float returnLossDb{45.0f};
    float returnLossAverage{45.0f};
    float powerSpread{0.0f};
    float swrSpread{0.0f};
    float returnLossSpread{0.0f};
    float severity{0.0f};
    float incidentSeverity{0.0f};
    QString incidentLabel;
    bool active{false};
    bool incident{false};
};

class HealthApplet : public QWidget {
    Q_OBJECT

public:
    explicit HealthApplet(QWidget* parent = nullptr);

    void setMeterModel(MeterModel* model);
    void setPowerScale(int maxWatts, bool hasAmplifier);

    void updateRadioMeters(float fwdPowerWatts, float swr);
    void updateTunerMeters(float fwdPowerWatts, float swr);
    void updateAmplifierMeters(float fwdPowerWatts, float swr);

private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void showEvent(QShowEvent* event) override;

    enum class MeterSource {
        None,
        Radio,
        Tuner,
        Amplifier
    };

    struct MeterSnapshot {
        float powerWatts{0.0f};
        float swrQualifyingPowerWatts{0.0f};
        float swr{1.0f};
        qint64 updatedAtMs{0};
        qint64 swrQualifyingPowerUpdatedAtMs{0};
        qint64 swrSampleUpdatedAtMs{0};
        bool valid{false};
    };

    void cacheMeters(MeterSource source, float fwdPowerWatts, float swr);
    void updateRadioDirectionalMeters(float forwardPowerWatts,
                                      float reflectedPowerWatts,
                                      float swr,
                                      bool reflectedPowerMeasured);
    MeterSnapshot bestSnapshot(qint64 nowMs, MeterSource* source) const;
    void appendFrame();
    void applyTheme();
    void togglePaused();
    void showPausedState();
    void updateStatusLabels(const AntennaHealthSample& sample, MeterSource source);
    QString sourceText(MeterSource source) const;
    QString formatPower(float watts) const;
    float computeSeverity(float powerWatts, float swr) const;
    void pushRecent(float powerWatts, float swr, float returnLossDb);
    void recomputeRecentStats();

    MeterModel* m_model{nullptr};
    QTimer* m_tickTimer{nullptr};
    HealthGraphWidget* m_graph{nullptr};
    QLabel* m_statusLabel{nullptr};
    QLabel* m_sourceLabel{nullptr};
    QLabel* m_scoreLabel{nullptr};
    QLabel* m_swrLabel{nullptr};
    QLabel* m_returnLossLabel{nullptr};
    QLabel* m_powerLabel{nullptr};
    QLabel* m_varianceLabel{nullptr};

    MeterSnapshot m_radioSnapshot;
    MeterSnapshot m_tunerSnapshot;
    MeterSnapshot m_ampSnapshot;
    MeterSource m_lastSource{MeterSource::None};

    QVector<AntennaHealthSample> m_history;
    QVector<float> m_recentPower;
    QVector<float> m_recentSwr;
    QVector<float> m_recentReturnLoss;
    QVector<float> m_swrAdmissionWindow;

    float m_displayPower{0.0f};
    float m_displaySwr{1.0f};
    float m_powerAverage{0.0f};
    float m_swrAverage{1.0f};
    float m_returnLossAverage{45.0f};
    float m_powerStdDev{0.0f};
    float m_swrStdDev{0.0f};
    float m_returnLossStdDev{0.0f};
    float m_swrSpan{0.0f};
    float m_returnLossSpan{0.0f};
    float m_lastSeverity{0.0f};
    float m_lastReturnLossDb{45.0f};
    float m_powerFullScale{120.0f};
    bool m_baselineReady{false};
    bool m_paused{false};
    MeterSource m_swrAdmissionSource{MeterSource::None};
    qint64 m_lastAdmittedSwrSampleMs{0};
    int m_swrInactiveFrames{0};
    int m_swrSourceMismatchFrames{0};
    int m_activeFrames{0};
    int m_idleFrames{0};
    int m_incidentCooldownFrames{0};
};

} // namespace AetherSDR
