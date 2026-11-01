#pragma once

#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
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
    // unprovisioned | starting | needs_login | needs_approval | running | error
    QString state;
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
    QString keyExpiry;               // RFC 3339 when the sign-in expires; empty: never
    // No tailnet owns the radio yet (never provisioned, or a QR sign-in that
    // never completed): provisioning needs no admin token.
    bool claimable{false};
};

// A provisioning reply. authUrl is set for a sign-in by QR code (no auth
// key): the URL to show until the radio is signed in. error is a failure the
// container hit after issuing the token, which the caller must still keep.
struct TailnetShimProvisionReply {
    QString adminToken;
    QString authUrl;
    QString error;
    TailnetShimStatus status;
};

// What GET /v1/login returns to the admin-token holder.
struct TailnetShimLogin {
    QString state;
    QString authUrl;     // set while the radio waits for a sign-in
    QString keyExpiry;
};

// Pure helpers, separated from the transport so they can be unit-tested
// without sockets.
namespace tailnetshim {
std::optional<TailnetShimStatus> parseStatus(const QByteArray& json);
// Parses a provisioning reply: {"admin_token", "auth_url"?, "error"?, "status"}.
std::optional<TailnetShimProvisionReply> parseProvisionReply(const QByteArray& json);
std::optional<TailnetShimLogin> parseLoginReply(const QByteArray& json);
// The sign-in URL if it is one to show: https, a host, no spaces, bounded.
// The host isn't checked, so a self-hosted control server's URL works too.
QString checkedAuthUrl(const QString& url);
// Whether a container version signs in by QR code (0.5.0 and later).
bool supportsQrSignIn(const QString& version);
// Whether the Remote Access window should start the QR sign-in without being
// asked: the radio belongs to no tailnet (never provisioned, or a QR sign-in
// abandoned with its token), the container can, nothing is in flight, and
// the keychain has answered, so the token it holds is known and can't
// arrive after a new one.
bool shouldSignInUnasked(const TailnetShimStatus& status, bool tokenLoaded, bool haveToken,
                         bool busy, bool lanReachable, bool alreadyTried);
// The token the window keeps when the keychain answers: one it already
// holds (issued by a sign-in meanwhile) wins over the read.
QString tokenAfterKeychainRead(const QString& held, const QString& read);
QString parseError(const QByteArray& json);
// An empty authKey is left out, which asks for sign-in by QR code.
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
// returned to the caller to store (TailnetShimTokenStore). Both cross the LAN
// in the clear (plain HTTP to the radio's LAN address, as RFC #6271 §6 and
// ruling D3 accept): the key is single-use and spent within seconds, and the
// API refuses callers that aren't on a private LAN.
class TailnetShimClient : public QObject {
    Q_OBJECT
public:
    explicit TailnetShimClient(QObject* parent = nullptr);

    void setRadioAddress(const QHostAddress& address) { m_address = address; }

    void fetchStatus();
    void fetchLogin(const QString& adminToken);
    void provision(const QString& authKey, const QString& hostname,
                   const QStringList& allow, const QStringList& routes,
                   bool shareDiscovered, const QString& adminToken);
    void setAllow(const QStringList& allow, const QString& adminToken);
    void setSharing(const QStringList& routes, bool shareDiscovered, const QString& adminToken);
    void signOut(const QString& adminToken);

signals:
    void statusReceived(const AetherSDR::TailnetShimStatus& status);
    void provisioned(const AetherSDR::TailnetShimProvisionReply& reply);
    void loginReceived(const AetherSDR::TailnetShimLogin& login);
    // operation is "status", "login", "provision", "allow", "sharing" or "signout". unauthorized
    // is true when the shim refused the admin token.
    void requestFailed(const QString& operation, const QString& message, bool unauthorized);

private:
    QNetworkReply* send(const QString& method, const QString& path,
                        const QByteArray& body, const QString& adminToken);
    void handle(QNetworkReply* reply, const QString& operation);

    QNetworkAccessManager m_nam;
    QHostAddress m_address;
    QPointer<QNetworkReply> m_statusReply;   // the status request in flight, if any
    QPointer<QNetworkReply> m_loginReply;    // the login request in flight, if any
};

}  // namespace AetherSDR
