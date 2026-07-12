#pragma once

#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QMap>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace AetherSDR {

struct DStarConfiguration {
    bool autoStart{false};
    QString executablePath;
    QString serialPort;
    QString myCall;
    QString myCallSuffix;
    QString urCall{QStringLiteral("CQCQCQ")};
    QString rpt1{QStringLiteral("DIRECT")};
    QString rpt2{QStringLiteral("DIRECT")};
    QString message;

    bool operator==(const DStarConfiguration&) const = default;
};

enum class DStarRouteOrigin {
    Direct,
    Repeater
};

enum class DStarRouteDestination {
    LocalCq,
    Station,
    RepeaterArea,
    Custom
};

struct DStarRouteRequest {
    DStarRouteOrigin origin{DStarRouteOrigin::Direct};
    DStarRouteDestination destination{DStarRouteDestination::LocalCq};
    QString accessRepeaterCallsign;
    QChar accessRepeaterModule{QLatin1Char('A')};
    QString destinationCallsign;
    QChar destinationRepeaterModule{QLatin1Char('A')};
    QString customUrCall{QStringLiteral("CQCQCQ")};
    QString customRpt1{QStringLiteral("DIRECT")};
    QString customRpt2{QStringLiteral("DIRECT")};

    bool operator==(const DStarRouteRequest&) const = default;
};

struct DStarResolvedRoute {
    QString urCall;
    QString rpt1;
    QString rpt2;

    bool operator==(const DStarResolvedRoute&) const = default;
};

struct DStarSerialDevice {
    enum class Verification {
        Candidate,
        Probing,
        Verified,
        Unavailable
    };

    QString path;
    QString label;
    int score{0};
    bool highConfidence{false};
    bool present{true};
    Verification verification{Verification::Candidate};
    QString detail;

    bool operator==(const DStarSerialDevice&) const = default;
};

struct DStarSerialPortMetadata {
    QString path;
    QString description;
    QString manufacturer;
    QString serialNumber;
    int vendorIdentifier{-1};
    int productIdentifier{-1};
    bool stablePath{false};
    bool present{true};
};

enum class DStarTrafficDirection {
    Receive,
    Transmit,
    System
};

struct DStarTrafficEntry {
    quint64 id{0};
    DStarTrafficDirection direction{DStarTrafficDirection::Receive};
    QDateTime timestampUtc;
    int sliceId{-1};
    QString myCall;
    QString myCallSuffix;
    QString urCall;
    QString rpt1;
    QString rpt2;
    QString message;
    bool complete{false};

    bool operator==(const DStarTrafficEntry&) const = default;
};

class DStarModel : public QObject
{
    Q_OBJECT

public:
    explicit DStarModel(QObject* parent = nullptr, bool autoSelectSerial = false);
    ~DStarModel() override;

    DStarConfiguration configuration(const QString& fallbackMyCall = {}) const;
    QString configurationError(const DStarConfiguration& config,
                               const QString& fallbackMyCall = {}) const;
    static bool resolveRoute(const DStarRouteRequest& request,
                             DStarResolvedRoute* route,
                             QString* error = nullptr);
    static DStarRouteRequest routeRequestForConfiguration(
        const DStarConfiguration& config);
    static QString runtimeSetCommand(const DStarConfiguration& config);
    bool setConfiguration(const DStarConfiguration& config,
                          const QString& fallbackMyCall = {},
                          QString* error = nullptr);

    QList<DStarSerialDevice> serialDevices() const { return m_serialDevices; }
    static DStarSerialDevice classifySerialDevice(
        const DStarSerialPortMetadata& metadata,
        const QString& configuredPort = {});
    static bool serialPathsEquivalent(const QString& lhs, const QString& rhs);
    static QString autoSelectedSerialPath(
        const QList<DStarSerialDevice>& devices,
        const QString& configuredPort);
    void refreshSerialDevices();

    const QList<DStarTrafficEntry>& traffic() const { return m_traffic; }
    void handleWaveformStatus(const QMap<QString, QString>& report);
    void clearTraffic();
    void setTrafficPersistencePath(const QString& path);

    bool start(const QHostAddress& radioAddress, const QString& radioCallsign);
    void stop();
    bool serviceActive() const;
    bool serviceStopping() const;
    QString serviceStateName() const;
    QString serviceStatusText() const;
    QString serviceLastError() const;
    QString serviceHealthName() const;
    QString serviceHealthDetail() const;
    bool registrationVerified() const;
    int activeSliceId() const;
    bool helperAvailable() const;
    QString vocoderLabel() const;

signals:
    void configurationChanged();
    void serialDevicesChanged();
    void trafficChanged();
    void serviceChanged();

private:
    static QString decodeStatusValue(const QString& value, int maximumLength);
    static QMap<QString, QString> normalizedReport(const QMap<QString, QString>& report);
    static bool reportHasAny(const QMap<QString, QString>& report,
                             const QStringList& keys);

    int trafficIndexForId(quint64 id) const;
    void appendTraffic(DStarTrafficEntry entry);
    void closeOpenReceive(int sliceId);
    void loadTraffic();
    void saveTraffic() const;
    void scheduleTrafficSave();
    void cancelSerialProbe();
    void startNextSerialProbe();
    void finishSerialProbe(bool verified, const QString& detail);
    void autoSelectVerifiedSerial();
    int serialDeviceIndex(const QString& path) const;

    QList<DStarSerialDevice> m_serialDevices;
    QList<DStarTrafficEntry> m_traffic;
    QHash<int, quint64> m_openReceiveBySlice;
    quint64 m_nextTrafficId{1};
    QString m_trafficPersistencePath;
    QTimer m_saveTimer;
    QProcess m_serialProbe;
    QStringList m_serialProbeQueue;
    QString m_activeSerialProbePath;
    QString m_serialProbeForcedError;
    quint64 m_serialProbeGeneration{0};
    quint64 m_activeSerialProbeGeneration{0};
    bool m_autoSelectSerial{false};

    static constexpr qsizetype kMaximumTrafficEntries = 500;
};

} // namespace AetherSDR
