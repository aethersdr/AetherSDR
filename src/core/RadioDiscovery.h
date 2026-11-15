#pragma once

#include "RadioSettingsIdentity.h"

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
    // aetherd Gap B (Step 2b): radio family discriminator. "flex" (default, so
    // every existing Flex discovery/probe path is unchanged) or "hl2"
    // (Hermes-Lite 2, from HPSDR discovery / the manual HL2 path). RadioModel
    // routes the connect through the matching backend based on this.
    QString family{QStringLiteral("flex")};
    QString name;           // e.g. "FLEX-6600"
    QString model;
    QString serial;
    RadioSerialIdentity serialIdentity; // Reported identity, independent of locator.
    QString version;
    // Display-only label for the version when a bare number isn't
    // self-describing (HL2 gateware "75" vs Flex "4.2.20.41343"). Empty renders the
    // version alone. Kept separate from `version`, which rigctl and the automation
    // bridge serve as a parseable token.
    QString versionLabel;
    QString nickname;
    QString callsign;
    QHostAddress address;
    quint16 port{4992};
    QString status;         // "Available" | "In_Use" | etc.
    int maxLicensedVersion{0};
    // Capacity from discovery keys `max_slices` / `max_panadapters` (#5594); 0 =
    // not reported (older firmware, connect by IP), fall back to the FlexLib model
    // table. Not `available_*`, which are the currently free counts (FlexLib keeps
    // all four apart, Discovery.cs:141/154/247/260). FLEX-8600 4.2.20.41343 reports
    // max/available 4/4 for both.
    int maxSlices{0};
    int maxPanadapters{0};
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

    // Couple discovery's re-bind loop to the connection (#3420). While connected
    // the 5 s re-bind churn stops. Local (remote=false): socket and stale sweep
    // stay so Multi-Flex broadcasts still refresh the list. Remote (VPN/SmartLink):
    // broadcasts can't reach us, so stop the sweep and release the socket.
    // Disconnect resumes discovery with a fresh retry budget.
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
