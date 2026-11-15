#pragma once

#include "PersistentDialog.h"

#include <QLabel>
#include <QTimer>
#include <QEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include "core/PropForecastClient.h"

class QNetworkAccessManager;
class QFrame;

namespace AetherSDR {

class PropDashboardDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit PropDashboardDialog(PropForecastClient* client, QWidget* parent = nullptr);

private:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void refresh();
    void onDetailUpdated(const PropForecastDetail& detail);
    void fetchImages();
    void updateLearningNotes();
    void updateVhfLearningNotes();
    static QString kpColor(double kp);

    PropForecastClient* m_client;
    QNetworkAccessManager* m_nam{nullptr};
    QTimer m_refreshTimer;

    static constexpr int kMetricCardCount = 5;
    QFrame* m_metricCards[kMetricCardCount]{};
    QLabel* m_metricValueLabels[kMetricCardCount]{};
    QLabel* m_metricDetailLabels[kMetricCardCount]{};
    static constexpr int kLearningNoteCount = 3;
    QLabel* m_learningNoteLabels[kLearningNoteCount]{};
    static constexpr int kVhfNoteCount = 2;
    QLabel* m_vhfNoteLabels[kVhfNoteCount]{};

    // Solar/Lunar imagery
    QLabel* m_solarImage{nullptr};
    QLabel* m_solarTypeLabel{nullptr};
    QLabel* m_lunarImage{nullptr};
    QLabel* m_lunarPhaseLabel{nullptr};
    QString m_lunarPhaseName;
    int m_lunarIllumination{-1};
    int m_solarTypeIndex{0};
    void fetchSolarImage();
    void cycleSolarImage();

    // Combined 3-day table: Kp + blackout + radiation
    QLabel* m_dayHeaders[3]{};
    QLabel* m_kpCells[3][8]{};
    QLabel* m_maxKpLabels[3]{};
    QLabel* m_r1r2Labels[3]{};
    QLabel* m_r3Labels[3]{};
    QLabel* m_s1Labels[3]{};
    QLabel* m_rationale{nullptr};

    // Band conditions
    QLabel* m_bandDayLabels[4]{};
    QLabel* m_bandNightLabels[4]{};
    QLabel* m_geoFieldLabel{nullptr};
    QLabel* m_noiseLabel{nullptr};
    QLabel* m_windLabel{nullptr};
    QLabel* m_xrayLabel{nullptr};

    // VHF conditions
    QLabel* m_auroraLabel{nullptr};
    QLabel* m_esNaLabel{nullptr};
    QLabel* m_esEuLabel{nullptr};
};

} // namespace AetherSDR
