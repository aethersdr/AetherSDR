#include "Ft8AgentClient.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace AetherSDR {

namespace {

constexpr int kTimeoutMs = 8000;
// An hour of FT8 cycles with their spots is a few hundred KB; anything much
// larger is not the receiver.
constexpr qint64 kMaxReplyBytes = 4 * 1024 * 1024;

}  // namespace

Ft8AgentClient::Ft8AgentClient(QObject* parent)
    : QObject(parent)
{
    m_nam.setTransferTimeout(kTimeoutMs);
    // The receiver never redirects; following one would carry the admin
    // token in its Authorization header to wherever it pointed.
    m_nam.setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
}

void Ft8AgentClient::setRadioAddress(const QHostAddress& address)
{
    if (address == m_address) {
        return;
    }
    cancel();
    m_address = address;
}

void Ft8AgentClient::cancel()
{
    ++m_generation;
    if (m_statusReply) {
        m_statusReply->abort();
    }
}

QNetworkReply* Ft8AgentClient::send(const QByteArray& method, const QString& pathAndQuery,
                                    const QByteArray& body, const QString& adminToken)
{
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(m_address.toString());
    url.setPort(kFt8Port);
    const qsizetype q = pathAndQuery.indexOf(QLatin1Char('?'));
    url.setPath(q < 0 ? pathAndQuery : pathAndQuery.left(q));
    if (q >= 0) {
        url.setQuery(pathAndQuery.mid(q + 1));
    }
    QNetworkRequest req(url);
    req.setTransferTimeout(kTimeoutMs);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!adminToken.isEmpty()) {
        req.setRawHeader("Authorization", "Bearer " + adminToken.toUtf8());
    }
    return method == "GET" ? m_nam.get(req) : m_nam.post(req, body);
}

void Ft8AgentClient::fetchStatus(qint64 since)
{
    if (!ft8::reachableAddress(m_address) || m_statusReply) {
        return;
    }
    m_statusReply = send("GET", QStringLiteral("/v1/status?since=%1").arg(since), {}, {});
    handle(m_statusReply, QStringLiteral("status"));
}

void Ft8AgentClient::fetchConfig()
{
    if (!ft8::reachableAddress(m_address)) {
        emit requestFailed(QStringLiteral("config"), tr("The radio's address can't reach the FT8 receiver."), false);
        return;
    }
    handle(send("GET", QStringLiteral("/v1/config"), {}, {}), QStringLiteral("config"));
}

void Ft8AgentClient::saveConfig(const Ft8Config& config, const QString& adminToken)
{
    if (!ft8::reachableAddress(m_address)) {
        emit requestFailed(QStringLiteral("save"), tr("The radio's address can't reach the FT8 receiver."), false);
        return;
    }
    handle(send("POST", QStringLiteral("/v1/config"), ft8::configJson(config), adminToken),
           QStringLiteral("save"));
}

void Ft8AgentClient::handle(QNetworkReply* reply, const QString& operation)
{
    const quint64 generation = m_generation;
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64) {
        if (received > kMaxReplyBytes) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, operation, generation] {
        reply->deleteLater();
        if (generation != m_generation) {
            return;   // retired by a new address or cancel()
        }
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        if (code == 0) {
            emit requestFailed(operation,
                               tr("The FT8 receiver did not answer (%1). It may still be starting.")
                                   .arg(reply->errorString()),
                               false);
            return;
        }
        if (code == 401) {
            emit requestFailed(operation, tr("The FT8 receiver refused this computer's admin token."), true);
            return;
        }
        if (code != 200) {
            const QString why = QString::fromUtf8(body.left(200)).simplified();
            emit requestFailed(operation, tr("The FT8 receiver answered %1: %2").arg(code).arg(why), false);
            return;
        }
        if (operation == QLatin1String("status")) {
            if (auto st = ft8::parseStatus(body)) {
                emit statusReceived(*st);
            } else {
                emit requestFailed(operation, tr("The FT8 receiver sent a status this AetherSDR can't read."), false);
            }
            return;
        }
        bool claimed = false;
        auto cfg = ft8::parseConfig(body, &claimed);
        if (!cfg) {
            emit requestFailed(operation, tr("The FT8 receiver sent settings this AetherSDR can't read."), false);
            return;
        }
        if (operation == QLatin1String("save")) {
            emit configSaved(*cfg, ft8::adminToken(body));
        } else {
            emit configReceived(*cfg, claimed);
        }
    });
}

}  // namespace AetherSDR
