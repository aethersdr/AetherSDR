#include "TailnetShimClient.h"

#include "TailnetShimRelease.h"
#include "TailnetShimCredentialPersistence.h"
#include "AetherPDTransportPolicy.h"

#include <QJsonArray>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>

namespace AetherSDR {

namespace {
// Provisioning waits for the shim's tailnet login, which takes seconds and
// up to the shim's own 90 s limit. Everything else answers at once, so it
// gets a short bound: the window polls status every 3 s, and a responder
// that accepts but never answers must not pile up 100 s requests.
constexpr int kTransferTimeoutMs = 100'000;
constexpr int kQuickTimeoutMs = 8'000;
// A status or provisioning reply is a few KB. Anything that answers on the
// radio's LAN address could send more; QNAM would buffer all of it before
// it reaches the parser.
constexpr qint64 kMaxReplyBytes = 256 * 1024;
}  // namespace

namespace tailnetshim {

QByteArray credentialFingerprint(const QString& token)
{
    return token.isEmpty() ? QByteArray{}
        : QCryptographicHash::hash("Bearer " + token.toUtf8(), QCryptographicHash::Sha256);
}

namespace {
// Status strings are shown in the window. Real ones are short; the caps
// keep a misbehaving or spoofed peer from filling it.
constexpr int kMaxField = 256;
constexpr int kMaxMessage = 512;
constexpr int kMaxEntries = 64;

QString field(const QJsonObject& o, const QString& key, int max = kMaxField)
{
    return o.value(key).toString().left(max);
}

QStringList fieldList(const QJsonObject& o, const QString& key)
{
    QStringList out;
    for (const QJsonValue& v : o.value(key).toArray()) {
        if (out.size() == kMaxEntries) {
            break;
        }
        out << v.toString().left(kMaxField);
    }
    return out;
}
}  // namespace

std::optional<TailnetShimStatus> statusFromObject(const QJsonObject& o)
{
    if (!o.contains(QStringLiteral("state")) || !o.contains(QStringLiteral("version"))) {
        return std::nullopt;
    }
    TailnetShimStatus st;
    st.version = field(o, QStringLiteral("version"));
    st.provisioned = o.value(QStringLiteral("provisioned")).toBool();
    st.state = field(o, QStringLiteral("state"));
    st.hostname = field(o, QStringLiteral("hostname"));
    st.installationId = field(o, QStringLiteral("installation_id"));
    if (!st.installationId.isEmpty()) {
        static const QRegularExpression identity(QStringLiteral("\\A[0-9a-f]{64}\\z"));
        if (!identity.match(st.installationId).hasMatch()
            || st.installationId == QString(64, QLatin1Char('0'))) {
            return std::nullopt;
        }
    }
    st.managedOwnerVersion = o.value(QStringLiteral("managed_owner_version")).toInt();
    st.ownerKeyVersion = o.value(QStringLiteral("owner_key_version")).toInt();
    st.tailnetIp = field(o, QStringLiteral("tailnet_ip"));
    st.dnsName = field(o, QStringLiteral("dns_name"));
    st.allow = fieldList(o, QStringLiteral("allow"));
    st.sessions = o.value(QStringLiteral("sessions")).toInt();
    st.lastError = field(o, QStringLiteral("last_error"), kMaxMessage);
    st.routes = fieldList(o, QStringLiteral("routes"));
    st.shareDiscovered = o.value(QStringLiteral("share_discovered")).toBool(true);
    for (const QJsonValue& v : o.value(QStringLiteral("discovered_devices")).toArray()) {
        if (st.discovered.size() == kMaxEntries) {
            break;
        }
        const QJsonObject d = v.toObject();
        TailnetShimDevice dev;
        dev.kind = field(d, QStringLiteral("kind"));
        dev.name = field(d, QStringLiteral("name"));
        dev.ip = field(d, QStringLiteral("ip"));
        dev.port = d.value(QStringLiteral("port")).toInt();
        if (!dev.ip.isEmpty()) {
            st.discovered << dev;
        }
    }
    st.advertisedRoutes = fieldList(o, QStringLiteral("advertised_routes"));
    st.approvedRoutes = fieldList(o, QStringLiteral("approved_routes"));
    st.keyExpiry = field(o, QStringLiteral("key_expiry"));
    // Before 0.5.0 there was no such field: only an unprovisioned radio was.
    st.claimable = o.contains(QStringLiteral("claimable"))
        ? o.value(QStringLiteral("claimable")).toBool()
        : !st.provisioned;
    return st;
}

std::optional<TailnetShimStatus> parseStatus(const QByteArray& json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        return std::nullopt;
    }
    return statusFromObject(doc.object());
}

std::optional<TailnetShimProvisionReply> parseProvisionReply(const QByteArray& json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        return std::nullopt;
    }
    const QJsonObject o = doc.object();
    const QString token = o.value(QStringLiteral("admin_token")).toString();
    const auto st = statusFromObject(o.value(QStringLiteral("status")).toObject());
    // The shim's tokens are 43 characters (32 random bytes, base64url). A
    // longer one is refused rather than stored and sent with every request.
    if (token.isEmpty() || token.size() > kMaxField || !st) {
        return std::nullopt;
    }
    TailnetShimProvisionReply reply;
    reply.adminToken = token;
    reply.authUrl = checkedAuthUrl(o.value(QStringLiteral("auth_url")).toString());
    reply.error = field(o, QStringLiteral("error"), kMaxMessage);
    reply.status = *st;
    return reply;
}

std::optional<TailnetShimLogin> parseLoginReply(const QByteArray& json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject() || !doc.object().contains(QStringLiteral("state"))) {
        return std::nullopt;
    }
    const QJsonObject o = doc.object();
    TailnetShimLogin login;
    login.state = field(o, QStringLiteral("state"));
    login.authUrl = checkedAuthUrl(o.value(QStringLiteral("auth_url")).toString());
    login.keyExpiry = field(o, QStringLiteral("key_expiry"));
    return login;
}

QString checkedAuthUrl(const QString& url)
{
    // Real ones are about 50 characters; the cap keeps the QR code readable.
    constexpr int kMaxUrl = 512;
    if (url.isEmpty() || url.size() > kMaxUrl) {
        return {};
    }
    const QUrl u(url, QUrl::StrictMode);
    if (!u.isValid() || u.scheme() != QLatin1String("https") || u.host().isEmpty()
        || url.contains(QRegularExpression(QStringLiteral("\\s")))) {
        return {};
    }
    return url;
}

bool supportsQrSignIn(const QString& version)
{
    // "0.5.0", "0.5.0-native": compare major.minor.
    const QRegularExpressionMatch m =
        QRegularExpression(QStringLiteral("^(\\d+)\\.(\\d+)")).match(version);
    if (!m.hasMatch()) {
        return false;
    }
    const int major = m.captured(1).toInt();
    const int minor = m.captured(2).toInt();
    return major > 0 || minor >= 5;
}

QString pinnedVersion(bool native)
{
    return QString::fromLatin1(native ? TailnetShimNativeRelease::kVersion
                                      : TailnetShimRelease::kVersion);
}

bool updateAvailable(const QString& installed, const QString& pinned)
{
    static const QRegularExpression re(QStringLiteral("^(\\d{1,6})\\.(\\d{1,6})\\.(\\d{1,6})"));
    const QRegularExpressionMatch a = re.match(installed);
    const QRegularExpressionMatch b = re.match(pinned);
    if (!a.hasMatch() || !b.hasMatch()) {
        return false;
    }
    for (int i = 1; i <= 3; ++i) {
        const int x = a.captured(i).toInt();
        const int y = b.captured(i).toInt();
        if (x != y) {
            return x < y;
        }
    }
    return false;
}

bool supportsManagedEnrollment(const TailnetShimStatus& st)
{
    return st.installationId.isEmpty() || st.managedOwnerVersion == 1;
}

bool supportsOwnerKeyReplacement(const TailnetShimStatus& st)
{
    return !st.installationId.isEmpty() && st.provisioned && !st.claimable
        && st.managedOwnerVersion == 1 && st.ownerKeyVersion == 1;
}

bool shouldSignInUnasked(const TailnetShimStatus& st, bool tokenLoaded, bool haveToken,
                         bool busy, bool lanReachable, bool alreadyTried)
{
    if (alreadyTried || busy || !lanReachable || !tokenLoaded || !supportsQrSignIn(st.version)
        || !supportsManagedEnrollment(st)) {
        return false;
    }
    const bool fresh = !st.provisioned && st.state == QLatin1String("unprovisioned");
    // A pending sign-in nobody here can finish: its token is elsewhere, or
    // this computer's attempt failed (error) before anyone signed in.
    const bool abandoned = st.claimable && st.provisioned
        && (!haveToken || st.state == QLatin1String("error"));
    // The installed local service already has an owner. Opening Remote
    // Access may enroll it only after that owner's keychain token is loaded.
    const bool localOwner = !st.installationId.isEmpty() && st.provisioned && !st.claimable
        && st.state == QLatin1String("local") && haveToken;
    return fresh || abandoned || localOwner;
}

QString tokenAfterKeychainRead(const QString& held, const QString& read)
{
    return held.isEmpty() ? read : held;
}

QString parseError(const QByteArray& json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (doc.isObject()) {
        const QString e = field(doc.object(), QStringLiteral("error"), kMaxMessage);
        if (!e.isEmpty()) {
            return e;
        }
    }
    return {};
}

QByteArray provisionBody(const QString& authKey, const QString& hostname,
                         const QStringList& allow, const QStringList& routes,
                         bool shareDiscovered)
{
    QJsonObject o;
    if (!authKey.trimmed().isEmpty()) {
        o.insert(QStringLiteral("auth_key"), authKey.trimmed());
    }
    o.insert(QStringLiteral("hostname"), hostname.trimmed());
    o.insert(QStringLiteral("allow"), QJsonArray::fromStringList(allow));
    o.insert(QStringLiteral("routes"), QJsonArray::fromStringList(routes));
    o.insert(QStringLiteral("share_discovered"), shareDiscovered);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QByteArray sharingBody(const QStringList& routes, bool shareDiscovered)
{
    QJsonObject o;
    o.insert(QStringLiteral("routes"), QJsonArray::fromStringList(routes));
    o.insert(QStringLiteral("share_discovered"), shareDiscovered);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QString suggestedHostname(const QString& nickname)
{
    QString h = nickname.toLower();
    h.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    h.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    if (h.size() > 63) {
        h = h.left(63);
        h.remove(QRegularExpression(QStringLiteral("-+$")));
    }
    return h.isEmpty() ? QStringLiteral("flex-radio") : h;
}

QStringList splitAllowList(const QString& text)
{
    QStringList out;
    for (const QString& part :
         text.split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts)) {
        if (!out.contains(part)) {
            out << part;
        }
    }
    return out;
}

}  // namespace tailnetshim

TailnetShimClient::TailnetShimClient(QObject* parent, QNetworkAccessManager* network)
    : QObject(parent)
    , m_network(network ? network : &m_nam)
{
    m_nam.setProxy(QNetworkProxy::NoProxy);
    m_nam.setTransferTimeout(kTransferTimeoutMs);
    // The API never redirects; following one would carry the admin token
    // in its Authorization header to wherever it pointed.
    m_nam.setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
}

TailnetShimClient::TailnetShimClient(QNetworkAccessManager& network, QObject* parent)
    : TailnetShimClient(parent, &network)
{}

std::shared_ptr<void> TailnetShimClient::acquireProvisioningLease(const QString& radioSerial)
{
    const QString serial = radioSerial.trimmed();
    if (serial.isEmpty() || serial.toUtf8().size() > 512) { return {}; }
    return tailnetshim::CredentialPersistence::systemAuthority()(QStringLiteral("TailnetShim/Provision/") + serial);
}

void TailnetShimClient::retirePendingReplies()
{
    ++m_replyGeneration;
    const QPointer<QNetworkReply> status = m_statusReply;
    const QPointer<QNetworkReply> login = m_loginReply;
    const QPointer<QNetworkReply> ownerKey = m_ownerKeyReply;
    m_statusReply.clear();
    m_loginReply.clear();
    m_ownerKeyReply.clear();
    if (status) { status->abort(); }
    if (login) { login->abort(); }
    if (ownerKey) { ownerKey->abort(); }
}

QNetworkReply* TailnetShimClient::send(const QString& method, const QString& path,
                                       const QByteArray& body, const QString& adminToken)
{
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(m_address.toString());
    url.setPort(kTailnetShimApiPort);
    url.setPath(path);
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setTransferTimeout(path == QLatin1String("/v1/provision") ? kTransferTimeoutMs
                                                                  : kQuickTimeoutMs);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!adminToken.isEmpty()) {
        req.setRawHeader("Authorization", "Bearer " + adminToken.toUtf8());
    }
    if (method == QLatin1String("GET")) {
        return m_network->get(req);
    }
    if (method == QLatin1String("PUT")) {
        return m_network->put(req, body);
    }
    return m_network->post(req, body);
}

void TailnetShimClient::handle(QNetworkReply* reply, const QString& operation,
                              std::optional<aetherpd::OwnerKeyReceipt> expectedOwner)
{
    const QByteArray authorization = reply->request().rawHeader("Authorization");
    const QByteArray credential = authorization.isEmpty() ? QByteArray{}
        : QCryptographicHash::hash(authorization, QCryptographicHash::Sha256);
    const quint64 generation = m_replyGeneration;
    const qint64 limit = expectedOwner ? 512 : kMaxReplyBytes;
    if (expectedOwner) { reply->setReadBufferSize(limit + 1); }
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply, limit](qint64 received, qint64) {
        if (received > limit) {
            reply->abort();
        }
    });
    if (expectedOwner) {
        connect(reply, &QIODevice::readyRead, reply, [reply, limit] {
            if (reply->bytesAvailable() > limit) { reply->abort(); }
        });
        connect(reply, &QNetworkReply::metaDataChanged, reply, [reply, limit] {
            bool valid = false;
            const qint64 size = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&valid);
            if (valid && size > limit) { reply->abort(); }
        });
    }
    connect(reply, &QNetworkReply::finished, this, [this, reply, operation, credential, generation, expectedOwner] {
        if (reply == m_statusReply) {
            m_statusReply = nullptr;
        }
        if (reply == m_loginReply) {
            m_loginReply = nullptr;
        }
        if (reply == m_ownerKeyReply) { m_ownerKeyReply = nullptr; }
        reply->deleteLater();
        if (generation != m_replyGeneration) {
            return;
        }
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        if (code == 0) {
            emit requestFailed(operation,
                               tr("Remote access did not answer on the radio (%1). It may "
                                  "still be starting.")
                                   .arg(reply->errorString()),
                               false, credential);
            return;
        }
        if (code < 200 || code >= 300) {
            QString message = tailnetshim::parseError(body);
            if (message.isEmpty()) {
                message = tr("Remote access on the radio returned HTTP %1.").arg(code);
            }
            emit requestFailed(operation, message, code == 401, credential);
            return;
        }
        if (operation == QLatin1String("owner-key")) {
            const std::optional<aetherpd::OwnerKeyReceipt> receipt = code == 200 && reply->error() == QNetworkReply::NoError
                ? aetherpd::parseOwnerKeyReceipt(body) : std::nullopt;
            if (!receipt || !expectedOwner || *receipt != *expectedOwner) {
                emit requestFailed(operation, tr("The radio did not confirm the exact saved owner credential. Keep both credentials."),
                                   false, credential);
                return;
            }
            emit ownerKeyConfirmed(*receipt);
            return;
        }
        if (operation == QLatin1String("provision")) {
            const auto parsed = tailnetshim::parseProvisionReply(body);
            if (!parsed) {
                emit requestFailed(operation, tr("Remote access on the radio sent an unreadable reply."), false, credential);
                return;
            }
            emit provisioned(*parsed);
            return;
        }
        if (operation == QLatin1String("login")) {
            const auto login = tailnetshim::parseLoginReply(body);
            if (!login) {
                emit requestFailed(operation, tr("Remote access on the radio sent an unreadable reply."), false, credential);
                return;
            }
            emit loginReceived(*login);
            return;
        }
        const auto st = tailnetshim::parseStatus(body);
        if (!st) {
            emit requestFailed(operation, tr("Remote access on the radio sent an unreadable reply."), false, credential);
            return;
        }
        if (operation == QLatin1String("signout")) {
            emit signedOut(*st);
        } else {
            emit statusReceived(*st);
        }
    });
}

void TailnetShimClient::fetchStatus()
{
    // One status request at a time: polling must not stack them up.
    if (m_statusReply) {
        return;
    }
    m_statusReply = send(QStringLiteral("GET"), QStringLiteral("/v1/status"), {}, {});
    handle(m_statusReply, QStringLiteral("status"));
}

void TailnetShimClient::fetchLogin(const QString& adminToken)
{
    if (m_loginReply || adminToken.isEmpty()) {
        return;
    }
    m_loginReply = send(QStringLiteral("GET"), QStringLiteral("/v1/login"), {}, adminToken);
    handle(m_loginReply, QStringLiteral("login"));
}

void TailnetShimClient::provision(const QString& authKey, const QString& hostname,
                                  const QStringList& allow, const QStringList& routes,
                                  bool shareDiscovered, const QString& adminToken)
{
    retirePendingReplies();
    handle(send(QStringLiteral("POST"), QStringLiteral("/v1/provision"),
                tailnetshim::provisionBody(authKey, hostname, allow, routes, shareDiscovered),
                adminToken),
           QStringLiteral("provision"));
}

void TailnetShimClient::setSharing(const QStringList& routes, bool shareDiscovered,
                                   const QString& adminToken)
{
    retirePendingReplies();
    handle(send(QStringLiteral("PUT"), QStringLiteral("/v1/routes"),
                tailnetshim::sharingBody(routes, shareDiscovered), adminToken),
           QStringLiteral("sharing"));
}

void TailnetShimClient::signOut(const QString& adminToken)
{
    retirePendingReplies();
    handle(send(QStringLiteral("POST"), QStringLiteral("/v1/signout"), {}, adminToken),
           QStringLiteral("signout"));
}

bool TailnetShimClient::replaceOwnerKey(const QString& installationId, const QString& savedCandidate,
                                       const QString& adminToken)
{
    const std::optional<QByteArray> body = aetherpd::ownerKeyBody(installationId, savedCandidate);
    if (!body || m_ownerKeyReply || !aetherpd::validAdminToken(adminToken) || !aetherpd::lanAddress(m_address)) { return false; }
    retirePendingReplies();
    const aetherpd::OwnerKeyReceipt expected{installationId,
        QCryptographicHash::hash(savedCandidate.toLatin1(), QCryptographicHash::Sha256)};
    m_ownerKeyReply = send(QStringLiteral("POST"), QStringLiteral("/v1/owner-key"), *body, adminToken);
    handle(m_ownerKeyReply, QStringLiteral("owner-key"), expected);
    return true;
}

}  // namespace AetherSDR
