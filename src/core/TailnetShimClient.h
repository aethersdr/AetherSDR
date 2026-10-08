#pragma once

#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>
#include <utility>

class QNetworkReply;

namespace AetherSDR {

// The in-radio tailnet shim's container name (its image label). The Waveforms
// dialog offers "Configure…" only for a Docker waveform with this name.
inline constexpr const char* kTailnetShimContainerName = "flex-tailnet-shim";

// TCP port of the shim's provisioning API on the radio's LAN address
// (tools/flex-tailnet-shim/api.go: ProvisionPort).
inline constexpr quint16 kTailnetShimApiPort = 48992;

// A 4O3A station accessory the container heard on the radio's LAN.
struct TailnetShimDevice {
    QString kind;   // "Antenna Genius", "Power Genius XL", "Tuner Genius XL"
    QString name;
    QString ip;
    int port{0};
};

// What GET /v1/status returns. Never contains a secret.
struct TailnetShimStatus {
    QString version;
    bool provisioned{false};
    QString state;       // unprovisioned | starting | running | error
    QString hostname;
    QString tailnetIp;
    QString dnsName;
    QStringList allow;
    int sessions{0};
    QString lastError;
    QStringList routes;              // extra LAN devices the operator listed
    bool shareDiscovered{true};      // share discovered station devices
    QList<TailnetShimDevice> discovered;
    QStringList advertisedRoutes;    // what the tailnet is offered now
    QStringList approvedRoutes;      // what the admin console has approved
};

// Pure helpers, separated from the transport so they can be unit-tested
// without sockets.
namespace tailnetshim {
std::optional<TailnetShimStatus> parseStatus(const QByteArray& json);
// Parses a provisioning reply: {"admin_token": "...", "status": {...}}.
std::optional<std::pair<QString, TailnetShimStatus>> parseProvisionReply(const QByteArray& json);
QString parseError(const QByteArray& json);
QByteArray provisionBody(const QString& authKey, const QString& hostname,
                         const QStringList& allow, const QStringList& routes = {},
                         bool shareDiscovered = true);
QByteArray allowBody(const QStringList& allow);
QByteArray sharingBody(const QStringList& routes, bool shareDiscovered);
// "KK7GWY FLEX-8600" -> "kk7gwy-flex-8600": a valid tailnet hostname.
QString suggestedHostname(const QString& nickname);
// Splits a comma- or whitespace-separated allowlist into entries.
QStringList splitAllowList(const QString& text);
}  // namespace tailnetshim

// Talks to the shim's provisioning API over the LAN. The auth key passes
// through provision() and is not retained anywhere. The admin token is
// returned to the caller to store (TailnetShimTokenStore), and is only
// placed in the Authorization header of a request.
class TailnetShimClient : public QObject {
    Q_OBJECT
public:
    explicit TailnetShimClient(QObject* parent = nullptr);

    void setRadioAddress(const QHostAddress& address) { m_address = address; }

    void fetchStatus();
    void provision(const QString& authKey, const QString& hostname,
                   const QStringList& allow, const QStringList& routes,
                   bool shareDiscovered, const QString& adminToken);
    void setAllow(const QStringList& allow, const QString& adminToken);
    void setSharing(const QStringList& routes, bool shareDiscovered, const QString& adminToken);
    void signOut(const QString& adminToken);

signals:
    void statusReceived(const AetherSDR::TailnetShimStatus& status);
    void provisioned(const QString& adminToken, const AetherSDR::TailnetShimStatus& status);
    // operation is "status", "provision", "allow", "sharing" or "signout". unauthorized
    // is true when the shim refused the admin token.
    void requestFailed(const QString& operation, const QString& message, bool unauthorized);

private:
    QNetworkReply* send(const QString& method, const QString& path,
                        const QByteArray& body, const QString& adminToken);
    void handle(QNetworkReply* reply, const QString& operation);

    QNetworkAccessManager m_nam;
    QHostAddress m_address;
};

}  // namespace AetherSDR
