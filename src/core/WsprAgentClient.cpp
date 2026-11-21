#include "WsprAgentClient.h"

#include "AppSettings.h"
#include "DxSpot.h"
#include "models/RadioModel.h"

#include <QDateTime>

namespace AetherSDR {

WsprAgentClient::WsprAgentClient(QObject* parent)
    : RadioAgentClient(kWsprRadioAgent, parent)
{
}

void WsprAgentClient::saveConfig(const WsprConfig& config, const QString& adminToken)
{
    postConfig(wspr::configJson(config), adminToken);
}

void WsprAgentClient::received(const QString& operation, const QByteArray& body)
{
    if (operation == QLatin1String("status")) {
        if (auto st = wspr::parseStatus(body)) {
            emit statusReceived(*st);
            emit receiverSlice(st->sliceIndex);
        } else {
            fail(operation, tr("The WSPR receiver sent a status this AetherSDR can't read."));
        }
        return;
    }
    bool claimed = false;
    auto cfg = wspr::parseConfig(body, &claimed);
    if (!cfg) {
        fail(operation, tr("The WSPR receiver sent settings this AetherSDR can't read."));
        return;
    }
    if (operation == QLatin1String("save")) {
        emit configSaved(*cfg, wspr::adminToken(body));
    } else {
        emit configReceived(*cfg, claimed);
    }
}

WsprSpotFeed::WsprSpotFeed(QObject* parent)
    : RadioAgentSpotFeed(new WsprAgentClient, parent)
{
    connect(static_cast<WsprAgentClient*>(client()), &WsprAgentClient::statusReceived,
            this, &WsprSpotFeed::onStatus);
}

qint64 WsprSpotFeed::firstCycle() const
{
    // Start with the spots still within their lifetime, so a restart puts
    // back what the panadapter showed rather than an empty band.
    const int lifetime = wspr::loadSpotHubSettings().lifetimeMinutes;
    return QDateTime::currentSecsSinceEpoch() - qint64(lifetime) * 60;
}

void WsprSpotFeed::onStatus(const WsprStatus& status)
{
    if (!isRunning()) {
        return;
    }
    emit statusReceived(status);
    m_receiverCall = status.callsign;
    m_receiverGrid = status.grid;
    // Read every poll, not cached: a color or lifetime changed in SpotHub
    // applies from the next poll.
    const WsprSpotHubSettings style = wspr::loadSpotHubSettings();
    const QList<WsprSpot> fresh = wspr::spotsSince(status.spots, m_since, &m_since);
    for (const WsprSpot& s : fresh) {
        emit decoded(s, status.grid);
        if (s.call.isEmpty()) {
            continue;   // an unresolved hashed call is no label
        }
        DxSpot d = wspr::toDxSpot(s, status.callsign);
        d.color = style.color;
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

WsprSpotHubSettings loadSpotHubSettings(const QString& key)
{
    return parseSpotHubSettings(AppSettings::instance().value(key).toString());
}

void saveSpotHubSettings(const WsprSpotHubSettings& settings, const QString& key)
{
    auto& s = AppSettings::instance();
    s.setValue(key, serializeSpotHubSettings(settings));
    s.save();
}

QMetaObject::Connection reserveReceiverSlice(WsprSpotFeed* feed, RadioModel* radio,
                                             const QString& label)
{
    return AetherSDR::reserveReceiverSlice(static_cast<RadioAgentSpotFeed*>(feed), radio, label);
}

}  // namespace wspr

}  // namespace AetherSDR
