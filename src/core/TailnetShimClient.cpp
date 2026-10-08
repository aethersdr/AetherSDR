#include "TailnetShimClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>

namespace AetherSDR {

namespace {
// Provisioning waits for the shim's tailnet login, which takes seconds and
// up to the shim's own 90 s limit; status is quick. One bound covers both.
constexpr int kTransferTimeoutMs = 100'000;
// A status or provisioning reply is a few KB. Anything that answers on the
// radio's LAN address could send more; QNAM would buffer all of it before
// it reaches the parser.
constexpr qint64 kMaxReplyBytes = 256 * 1024;
}  // namespace

namespace tailnetshim {

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

std::optional<std::pair<QString, TailnetShimStatus>> parseProvisionReply(const QByteArray& json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        return std::nullopt;
    }
    const QJsonObject o = doc.object();
    const QString token = o.value(QStringLiteral("admin_token")).toString();
    const auto st = statusFromObject(o.value(QStringLiteral("status")).toObject());
    if (token.isEmpty() || !st) {
        return std::nullopt;
    }
    return std::make_pair(token, *st);
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
    o.insert(QStringLiteral("auth_key"), authKey.trimmed());
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

QByteArray allowBody(const QStringList& allow)
{
    QJsonObject o;
    o.insert(QStringLiteral("allow"), QJsonArray::fromStringList(allow));
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

TailnetShimClient::TailnetShimClient(QObject* parent)
    : QObject(parent)
{
    m_nam.setTransferTimeout(kTransferTimeoutMs);
    // The API never redirects; following one would carry the admin token
    // in its Authorization header to wherever it pointed.
    m_nam.setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
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
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!adminToken.isEmpty()) {
        req.setRawHeader("Authorization", "Bearer " + adminToken.toUtf8());
    }
    if (method == QLatin1String("GET")) {
        return m_nam.get(req);
    }
    if (method == QLatin1String("PUT")) {
        return m_nam.put(req, body);
    }
    return m_nam.post(req, body);
}

void TailnetShimClient::handle(QNetworkReply* reply, const QString& operation)
{
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64) {
        if (received > kMaxReplyBytes) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, operation] {
        reply->deleteLater();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        if (code == 0) {
            emit requestFailed(operation,
                               tr("The remote-access container did not answer on the radio "
                                  "(%1). It may still be starting.")
                                   .arg(reply->errorString()),
                               false);
            return;
        }
        if (code < 200 || code >= 300) {
            QString message = tailnetshim::parseError(body);
            if (message.isEmpty()) {
                message = tr("The remote-access container returned HTTP %1.").arg(code);
            }
            emit requestFailed(operation, message, code == 401);
            return;
        }
        if (operation == QLatin1String("provision")) {
            const auto parsed = tailnetshim::parseProvisionReply(body);
            if (!parsed) {
                emit requestFailed(operation, tr("The container sent an unreadable reply."), false);
                return;
            }
            emit provisioned(parsed->first, parsed->second);
            return;
        }
        const auto st = tailnetshim::parseStatus(body);
        if (!st) {
            emit requestFailed(operation, tr("The container sent an unreadable reply."), false);
            return;
        }
        emit statusReceived(*st);
    });
}

void TailnetShimClient::fetchStatus()
{
    handle(send(QStringLiteral("GET"), QStringLiteral("/v1/status"), {}, {}),
           QStringLiteral("status"));
}

void TailnetShimClient::provision(const QString& authKey, const QString& hostname,
                                  const QStringList& allow, const QStringList& routes,
                                  bool shareDiscovered, const QString& adminToken)
{
    handle(send(QStringLiteral("POST"), QStringLiteral("/v1/provision"),
                tailnetshim::provisionBody(authKey, hostname, allow, routes, shareDiscovered),
                adminToken),
           QStringLiteral("provision"));
}

void TailnetShimClient::setSharing(const QStringList& routes, bool shareDiscovered,
                                   const QString& adminToken)
{
    handle(send(QStringLiteral("PUT"), QStringLiteral("/v1/routes"),
                tailnetshim::sharingBody(routes, shareDiscovered), adminToken),
           QStringLiteral("sharing"));
}

void TailnetShimClient::setAllow(const QStringList& allow, const QString& adminToken)
{
    handle(send(QStringLiteral("PUT"), QStringLiteral("/v1/allow"),
                tailnetshim::allowBody(allow), adminToken),
           QStringLiteral("allow"));
}

void TailnetShimClient::signOut(const QString& adminToken)
{
    handle(send(QStringLiteral("POST"), QStringLiteral("/v1/signout"), {}, adminToken),
           QStringLiteral("signout"));
}

}  // namespace AetherSDR
