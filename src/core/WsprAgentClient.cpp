#include "WsprAgentClient.h"

#include "AppSettings.h"
#include "DxSpot.h"
#include "ThemeManager.h"

#include <QDateTime>
#include <QTimer>

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace AetherSDR {

namespace {

constexpr int kTimeoutMs = 8000;
// A full day of spots is a few hundred KB; anything much larger is not the
// receiver.
constexpr qint64 kMaxReplyBytes = 4 * 1024 * 1024;

}  // namespace

WsprAgentClient::WsprAgentClient(QObject* parent)
    : QObject(parent)
{
    m_nam.setTransferTimeout(kTimeoutMs);
    // The receiver never redirects; following one would carry the admin
    // token in its Authorization header to wherever it pointed.
    m_nam.setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
}

void WsprAgentClient::setRadioAddress(const QHostAddress& address)
{
    if (address == m_address) {
        return;
    }
    cancel();
    m_address = address;
}

void WsprAgentClient::cancel()
{
    ++m_generation;
    if (m_statusReply) {
        m_statusReply->abort();
    }
}

QNetworkReply* WsprAgentClient::send(const QByteArray& method, const QString& pathAndQuery,
                                     const QByteArray& body, const QString& adminToken)
{
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(m_address.toString());
    url.setPort(kWsprPort);
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

void WsprAgentClient::fetchStatus(qint64 since)
{
    if (!wspr::reachableAddress(m_address) || m_statusReply) {
        return;
    }
    m_statusReply = send("GET", QStringLiteral("/v1/status?since=%1").arg(since), {}, {});
    handle(m_statusReply, QStringLiteral("status"));
}

void WsprAgentClient::fetchConfig()
{
    if (!wspr::reachableAddress(m_address)) {
        emit requestFailed(QStringLiteral("config"), tr("The radio's address can't reach the WSPR receiver."), false);
        return;
    }
    handle(send("GET", QStringLiteral("/v1/config"), {}, {}), QStringLiteral("config"));
}

void WsprAgentClient::saveConfig(const WsprConfig& config, const QString& adminToken)
{
    if (!wspr::reachableAddress(m_address)) {
        emit requestFailed(QStringLiteral("save"), tr("The radio's address can't reach the WSPR receiver."), false);
        return;
    }
    handle(send("POST", QStringLiteral("/v1/config"), wspr::configJson(config), adminToken),
           QStringLiteral("save"));
}

void WsprAgentClient::handle(QNetworkReply* reply, const QString& operation)
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
                               tr("The WSPR receiver did not answer (%1). It may still be starting.")
                                   .arg(reply->errorString()),
                               false);
            return;
        }
        if (code == 401) {
            emit requestFailed(operation, tr("The WSPR receiver refused this computer's admin token."), true);
            return;
        }
        if (code != 200) {
            const QString why = QString::fromUtf8(body.left(200)).simplified();
            emit requestFailed(operation, tr("The WSPR receiver answered %1: %2").arg(code).arg(why), false);
            return;
        }
        if (operation == QLatin1String("status")) {
            if (auto st = wspr::parseStatus(body)) {
                emit statusReceived(*st);
            } else {
                emit requestFailed(operation, tr("The WSPR receiver sent a status this AetherSDR can't read."), false);
            }
            return;
        }
        bool claimed = false;
        auto cfg = wspr::parseConfig(body, &claimed);
        if (!cfg) {
            emit requestFailed(operation, tr("The WSPR receiver sent settings this AetherSDR can't read."), false);
            return;
        }
        if (operation == QLatin1String("save")) {
            emit configSaved(*cfg, wspr::adminToken(body));
        } else {
            emit configReceived(*cfg, claimed);
        }
    });
}

WsprSpotFeed::WsprSpotFeed(QObject* parent)
    : QObject(parent)
    , m_client(new WsprAgentClient(this))
    , m_timer(new QTimer(this))
{
    m_timer->setInterval(30000);
    connect(m_timer, &QTimer::timeout, this, &WsprSpotFeed::poll);
    connect(m_client, &WsprAgentClient::statusReceived, this, &WsprSpotFeed::onStatus);
    connect(m_client, &WsprAgentClient::requestFailed, this,
            [this](const QString&, const QString& message, bool) { emit failed(message); });
}

void WsprSpotFeed::start(const QHostAddress& radio)
{
    stop();
    if (!wspr::reachableAddress(radio)) {
        emit failed(tr("The radio's address can't reach the WSPR receiver."));
        return;
    }
    if (radio != m_client->radioAddress()) {
        m_recent.clear();   // another radio's decodes
    }
    m_client->setRadioAddress(radio);
    const int lifetime = wspr::loadSpotHubSettings().lifetimeMinutes;
    m_since = QDateTime::currentSecsSinceEpoch() - qint64(lifetime) * 60;
    m_running = true;
    m_timer->start();
    emit started();
    poll();
}

void WsprSpotFeed::stop()
{
    if (!m_running) {
        return;
    }
    m_running = false;
    m_timer->stop();
    m_client->cancel();
    emit stopped();
}

void WsprSpotFeed::poll()
{
    if (m_running) {
        m_client->fetchStatus(m_since);
    }
}

void WsprSpotFeed::onStatus(const WsprStatus& status)
{
    if (!m_running) {
        return;
    }
    emit statusReceived(status);
    m_receiverCall = status.callsign;
    m_receiverGrid = status.grid;
    const WsprSpotHubSettings style = wspr::loadSpotHubSettings();
    const QList<WsprSpot> fresh = wspr::spotsSince(status.spots, m_since, &m_since);
    for (const WsprSpot& s : fresh) {
        emit decoded(s, status.grid);
        if (s.call.isEmpty()) {
            continue;   // an unresolved hashed call is no label
        }
        DxSpot d = wspr::toDxSpot(s, status.callsign);
        d.color = wspr::spotColor(style);
        d.lifetimeSec = style.lifetimeMinutes * 60;
        emit spotReceived(d);
    }
    if (!fresh.isEmpty()) {
        m_recent.append(fresh);
        constexpr qsizetype kMaxRecent = 2000;   // as many as the receiver keeps
        if (m_recent.size() > kMaxRecent) {
            m_recent.remove(0, m_recent.size() - kMaxRecent);
        }
        emit spotsUpdated();
    }
}

namespace wspr {

WsprSpotHubSettings loadSpotHubSettings()
{
    return parseSpotHubSettings(AppSettings::instance().value(QStringLiteral("WsprSpots")).toString());
}

void saveSpotHubSettings(const WsprSpotHubSettings& settings)
{
    auto& s = AppSettings::instance();
    s.setValue(QStringLiteral("WsprSpots"), serializeSpotHubSettings(settings));
    s.save();
}

QString spotColor(const WsprSpotHubSettings& settings)
{
    return settings.color.isEmpty() ? ThemeManager::instance().value(QStringLiteral("color.canon.category.violet"))
                                    : settings.color;
}

}  // namespace wspr

}  // namespace AetherSDR
