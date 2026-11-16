#pragma once

#include "core/backends/RadioControlTypes.h"
#include "core/AetherPDMeasurementSettings.h"
#include "core/txlinearity/TxLinearityController.h"
#include "core/txlinearity/TxLinearityTelemetry.h"

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QNetworkAccessManager;
namespace AetherSDR {
class RadioModel;
class SliceModel;
namespace txlin { class AetherPDCaptureSource; }

// One radio-owned measurement controller. Closing a panel requests Stop; it
// must not discard this model while qualified RF release is still pending.
class AetherPDMeasurementModel final : public QObject {
    Q_OBJECT
public:
    using State = txlin::TxLinearityController::State;
    struct Result {
        txlin::LinearityRunResult measurement;
        txlin::LinearityRunConfig config;
        txlin::RunRequest request;
        QString serial;
        QString mode;
        QString transmitAntenna;
        int powerSetting{0}; // Radio-reported nominal watts, not measured output.
        QDateTime completedAtUtc;
        quint64 sourceId{0};
        QByteArray toCsv() const;
        QString sourceSummary() const;
    };
    explicit AetherPDMeasurementModel(RadioModel& radio);
    AetherPDMeasurementModel(RadioModel& radio, QNetworkAccessManager& network);
    ~AetherPDMeasurementModel() override;
    bool prepare(const RadioControlSession& session, const txlin::LinearityRunConfig& config,
                 const txlin::RunRequest& request, bool dummyLoadConfirmed);
    bool prepareCurrent(const RadioControlSession& session, double maxForwardWatts, bool dummyLoadConfirmed);
    using AnalysisOptions = AetherPDMeasurementSettings::Values;
    struct PowerControl {
        std::optional<int> reported;
        std::optional<int> maximum;
        QString unavailableReason;
    };
    PowerControl powerControl(const RadioControlSession& session) const;
    bool requestPower(const RadioControlSession& session, int nominalWatts);
    AetherPDMeasurementSettings::ReadResult analysisOptions() const;
    bool setAnalysisOptions(const RadioControlSession& session, const AnalysisOptions& options);
    QByteArray exportResult() const;
    QByteArray exportResultCsv() const;
    QString resultSummary() const;
    QString preparationSummary() const;
    bool start(const RadioControlSession& session, std::function<bool()> operatorCurrent = {});
    bool stop(const RadioControlSession& session);
    void poll();
    quint64 preparationId() const { return m_generation; }
    State state() const;
    QString status() const;
    const std::optional<Result>& result() const { return m_result; }

signals:
    void changed();

private:
    void initialize(QNetworkAccessManager* network);
    bool current() const;
    void publish();
    QPointer<RadioModel> m_radio;
    QPointer<QNetworkAccessManager> m_network;
    bool m_injectedNetwork{false};
    bool m_shuttingDown{false};
    QPointer<txlin::AetherPDCaptureSource> m_capture;
    std::unique_ptr<txlin::TxLinearityController> m_controller;
    txlin::TxLinearityTelemetry m_telemetry;
    RadioControlSession m_session{};
    std::function<bool()> m_current;
    std::function<bool()> m_operatorCurrent;
    QPointer<SliceModel> m_slice;
    std::vector<QMetaObject::Connection> m_stationConnections;
    quint64 m_generation{0};
    quint64 m_measurementSourceId{0};
    double m_frequencyMHz{0};
    QString m_mode;
    QString m_antenna;
    QString m_serial;
    int m_powerSetting{0};
    txlin::LinearityRunConfig m_config;
    txlin::RunRequest m_request;
    QTimer m_observer;
    State m_publishedState{State::Idle};
    QString m_notice;
    std::optional<QString> m_bandPlanName;
    QString m_licenseClass;
    std::optional<Result> m_result;
};
} // namespace AetherSDR
