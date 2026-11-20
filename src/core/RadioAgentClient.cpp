#include "RadioAgentClient.h"

#include "models/RadioModel.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPair>
#include <QTimer>
#include <QUrl>

namespace AetherSDR {

namespace {

constexpr int kTimeoutMs = 8000;
// A full day of WSPR spots, or an hour of FT8 cycles with theirs, is a few
// hundred KB; anything much larger is not the receiver.
constexpr qint64 kMaxReplyBytes = 4 * 1024 * 1024;

}  // namespace

bool radioAgentReachable(const QHostAddress& address)
{
    if (address.isNull() || address.protocol() != QAbstractSocket::IPv4Protocol) {
        return false;
    }
    static const QPair<QHostAddress, int> tailnet = QHostAddress::parseSubnet(QStringLiteral("100.64.0.0/10"));
    return address.isPrivateUse() || address.isLinkLocal() || address.isInSubnet(tailnet);
}

RadioAgentClient::RadioAgentClient(const RadioAgent& agent, QObject* parent)
    : QObject(parent)
    , m_agent(agent)
{
    m_nam.setTransferTimeout(kTimeoutMs);
    // A receiver never redirects; following one would carry the admin token
    // in its Authorization header to wherever it pointed.
    m_nam.setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
}

QString RadioAgentClient::receiverName() const
{
    return tr("The %1 receiver").arg(QString::fromLatin1(m_agent.name));
}

void RadioAgentClient::setRadioAddress(const QHostAddress& address)
{
    if (address == m_address) {
        return;
    }
    cancel();
    m_address = address;
}

void RadioAgentClient::cancel()
{
    ++m_generation;
    if (m_statusReply) {
        m_statusReply->abort();
    }
}

void RadioAgentClient::fail(const QString& operation, const QString& message, bool unauthorized)
{
    emit requestFailed(operation, message, unauthorized);
}

QNetworkReply* RadioAgentClient::send(const QByteArray& method, const QString& pathAndQuery,
                                      const QByteArray& body, const QString& adminToken)
{
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(m_address.toString());
    url.setPort(m_agent.port);
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

void RadioAgentClient::fetchStatus(qint64 since)
{
    if (!radioAgentReachable(m_address) || m_statusReply) {
        return;
    }
    m_statusReply = send("GET", QStringLiteral("/v1/status?since=%1").arg(since), {}, {});
    handle(m_statusReply, QStringLiteral("status"));
}

void RadioAgentClient::fetchConfig()
{
    if (!radioAgentReachable(m_address)) {
        fail(QStringLiteral("config"), tr("The radio's address can't reach the %1 receiver.").arg(QString::fromLatin1(m_agent.name)));
        return;
    }
    handle(send("GET", QStringLiteral("/v1/config"), {}, {}), QStringLiteral("config"));
}

void RadioAgentClient::postConfig(const QByteArray& json, const QString& adminToken)
{
    if (!radioAgentReachable(m_address)) {
        fail(QStringLiteral("save"), tr("The radio's address can't reach the %1 receiver.").arg(QString::fromLatin1(m_agent.name)));
        return;
    }
    handle(send("POST", QStringLiteral("/v1/config"), json, adminToken), QStringLiteral("save"));
}

void RadioAgentClient::handle(QNetworkReply* reply, const QString& operation)
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
            fail(operation, tr("%1 did not answer (%2). It may still be starting.")
                                .arg(receiverName(), reply->errorString()));
            return;
        }
        if (code == 401) {
            fail(operation, tr("%1 refused this computer's admin token.").arg(receiverName()), true);
            return;
        }
        if (code != 200) {
            const QString why = QString::fromUtf8(body.left(200)).simplified();
            fail(operation, tr("%1 answered %2: %3").arg(receiverName()).arg(code).arg(why));
            return;
        }
        received(operation, body);
    });
}

RadioAgentSpotFeed::RadioAgentSpotFeed(RadioAgentClient* client, QObject* parent)
    : QObject(parent)
    , m_client(client)
    , m_timer(new QTimer(this))
{
    m_client->setParent(this);
    m_timer->setInterval(m_client->agent().pollMs);
    connect(m_timer, &QTimer::timeout, this, &RadioAgentSpotFeed::poll);
    connect(m_client, &RadioAgentClient::requestFailed, this,
            [this](const QString&, const QString& message, bool) { emit failed(message); });
    connect(m_client, &RadioAgentClient::receiverSlice, this, [this](int sliceIndex) {
        if (m_running) {
            emit receiverSlice(sliceIndex);
        }
    });
}

void RadioAgentSpotFeed::start(const QHostAddress& radio)
{
    stop();
    if (!radioAgentReachable(radio)) {
        emit failed(tr("The radio's address can't reach the %1 receiver.")
                        .arg(QString::fromLatin1(m_client->agent().name)));
        return;
    }
    if (radio != m_client->radioAddress()) {
        forgetRecent();   // another radio's decodes
    }
    m_client->setRadioAddress(radio);
    m_since = firstCycle();
    m_running = true;
    m_timer->start();
    emit started();
    poll();
}

void RadioAgentSpotFeed::stop()
{
    if (!m_running) {
        return;
    }
    m_running = false;
    m_timer->stop();
    m_client->cancel();
    emit stopped();
}

void RadioAgentSpotFeed::poll()
{
    if (m_running) {
        m_client->fetchStatus(m_since);
    }
}

QMetaObject::Connection reserveReceiverSlice(RadioAgentSpotFeed* feed, RadioModel* radio,
                                             const QString& label)
{
    return QObject::connect(feed, &RadioAgentSpotFeed::receiverSlice, radio,
                            [radio, label](int sliceIndex) {
        if (sliceIndex >= 0 && radio->isConnected() && !radio->isSliceReserved(sliceIndex)) {
            radio->reserveSlice(sliceIndex, label);
        }
    });
}

}  // namespace AetherSDR
