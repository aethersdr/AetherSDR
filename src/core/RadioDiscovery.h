#pragma once

#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include <QList>
#include <QMap>
#include <QString>
#include <QHostAddress>

namespace AetherSDR {

enum class RadioBindMode : quint8 {
    Auto,
    Explicit
};

struct RadioBindSettings {
    RadioBindMode mode{RadioBindMode::Auto};
    QHostAddress  bindAddress;
    QString       interfaceId;
    QString       interfaceName;

    bool hasBindableAddress() const
    {
        return !bindAddress.isNull() && bindAddress.protocol() == QAbstractSocket::IPv4Protocol;
    }

    QString modeString() const
    {
        return mode == RadioBindMode::Explicit ? QStringLiteral("Explicit")
                                               : QStringLiteral("Auto");
    }

    QString selectionLabel() const
    {
        if (mode == RadioBindMode::Auto)
            return QStringLiteral("Auto");

        QString iface = interfaceName.trimmed();
        if (iface.isEmpty())
            iface = interfaceId.trimmed();
        if (iface.isEmpty())
            return bindAddress.toString();
        if (bindAddress.isNull())
            return iface;
        return QStringLiteral("%1 (%2)").arg(iface, bindAddress.toString());
    }
};

// Represents a discovered FlexRadio on the network.
struct RadioInfo {
    QString name;           // e.g. "FLEX-6600"
    QString model;
    QString serial;
    QString version;
    QString nickname;
    QString callsign;
    QHostAddress address;
    quint16 port{4992};
    QString status;         // "Available" | "In_Use" | etc.
    int maxLicensedVersion{0};
    bool inUse{false};
    bool multiFlexEnabled{true}; // mf_enable from discovery; true = multi-client allowed
    bool isRouted{false};
    bool isSystemModel{false};
    QString turfRegion;
    // Optional: bands the radio itself supports, e.g. "2m,440,23cm"
    // (names from BandDefs).  Real Flex radios don't send this — band
    // capability then derives from the model string as before.  Gateways
    // presenting non-Flex hardware use it to declare their true band set
    // instead of inheriting the impersonated model's bands.
    QString bands;
    RadioBindSettings bindSettings;
    QHostAddress sessionBindAddress;

    // Connected GUI client info (from discovery broadcast)
    QStringList guiClientStations;
    QStringList guiClientHandles;
    QStringList guiClientPrograms;
    QStringList guiClientIps;
    QStringList guiClientHosts;

    QString displayName() const {
        QString suffix;
        if (!guiClientStations.isEmpty()) {
            const QString& station = guiClientStations.first();
            suffix = QString("Multi-Flex: %1").arg(station.isEmpty() ? "unknown" : station);
        } else {
            suffix = isRouted ? "routed" : "Local";
        }
        if (nickname.isEmpty() && callsign.isEmpty())
            return QString("%1 @ %2\nAvailable (%3)")
                .arg(model, address.toString(), suffix);
        return QString("%1  %2  %3\nAvailable (%4)")
            .arg(model, nickname, callsign, suffix);
    }
};

// Listens for SmartSDR discovery broadcasts on UDP port 4992
// and emits radioDiscovered / radioLost signals as radios appear/disappear.
class RadioDiscovery : public QObject {
    Q_OBJECT

public:
    static constexpr quint16 DISCOVERY_PORT  = 4992;
    static constexpr int STALE_TIMEOUT_MS   = 5000;  // radio considered gone after 5s
    static constexpr int BIND_RETRY_MS      = 2000;  // retry interval when bind fails
    static constexpr int MAX_BIND_RETRIES   = 15;    // give up after 30s (15 × 2s)
    static constexpr int REBIND_INTERVAL_MS = 5000;  // re-bind interval until first packet received
    static constexpr int MAX_REBIND_RETRIES = 12;    // give up re-bind after 60s (12 × 5s)

    explicit RadioDiscovery(QObject* parent = nullptr);
    ~RadioDiscovery() override;

    void startListening();
    void stopListening();

    // Couple discovery's re-bind loop to the connection lifecycle.  When a
    // radio is connected we stop the 5-second re-bind churn for the rest of
    // the session (#3420).  `remote` selects how aggressively to quiesce:
    //   • local (remote=false): keep the socket bound and the stale sweep
    //     running so Multi-Flex / other-GUI-client broadcasts still refresh
    //     the radio list passively; only the re-bind churn stops.
    //   • routed/VPN/SmartLink (remote=true): local UDP broadcasts cannot
    //     reach us by design, so passive listening buys nothing — fully
    //     quiesce by stopping the stale sweep and releasing the socket.
    // On disconnect, discovery resumes (re-bind with a fresh retry budget) so
    // the next connection — possibly a different local radio — can be found.
    void setConnected(bool connected, bool remote);

    QList<RadioInfo> discoveredRadios() const { return m_radios; }

signals:
    void radioDiscovered(const RadioInfo& radio);
    void radioUpdated(const RadioInfo& radio);
    void radioLost(const QString& serial);

private slots:
    void onReadyRead();
    void onStaleCheck();
    void onBindRetry();

private:
#ifdef AETHERSDR_TESTING
    friend class RadioDiscoveryParserTest;
#endif
    RadioInfo parseDiscoveryPacket(const QByteArray& data) const;
    void upsertRadio(const RadioInfo& info);

    QUdpSocket        m_socket;
    QTimer            m_staleTimer;
    QTimer            m_bindRetryTimer;  // retries bind if first attempt fails (e.g. macOS net consent)
    QTimer            m_rebindTimer;     // periodic re-bind until first packet (handles interface changes)
    int               m_bindRetryCount{0};
    int               m_rebindAttempts{0}; // count of re-bind firings, capped at MAX_REBIND_RETRIES (#3420)
    bool              m_receivedAny{false};
    bool              m_connected{false};  // active radio connection — pauses re-bind churn (#3420)
    QList<RadioInfo>  m_radios;

    // Track last-seen time per serial for staleness detection
    QMap<QString, qint64> m_lastSeen;
};

} // namespace AetherSDR
