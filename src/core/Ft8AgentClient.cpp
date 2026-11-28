#include "Ft8AgentClient.h"

#include "DxSpot.h"
#include "WsprAgentClient.h"
#include "models/RadioModel.h"

#include <QDateTime>

namespace AetherSDR {

Ft8AgentClient::Ft8AgentClient(QObject* parent)
    : RadioAgentClient(kFt8RadioAgent, parent)
{
}

void Ft8AgentClient::saveConfig(const Ft8Config& config, const QString& adminToken)
{
    postConfig(ft8::configJson(config), adminToken);
}

void Ft8AgentClient::received(const QString& operation, const QByteArray& body)
{
    if (operation == QLatin1String("status")) {
        if (auto st = ft8::parseStatus(body)) {
            emit statusReceived(*st);
            emit receiverSlice(st->sliceIndex);
        } else {
            fail(operation, tr("The FT8 receiver sent a status this AetherSDR can't read."));
        }
        return;
    }
    bool claimed = false;
    auto cfg = ft8::parseConfig(body, &claimed);
    if (!cfg) {
        fail(operation, tr("The FT8 receiver sent settings this AetherSDR can't read."));
        return;
    }
    if (operation == QLatin1String("save")) {
        emit configSaved(*cfg, ft8::adminToken(body));
    } else {
        emit configReceived(*cfg, claimed);
    }
}

Ft8SpotFeed::Ft8SpotFeed(QObject* parent)
    : RadioAgentSpotFeed(new Ft8AgentClient, parent)
{
    connect(static_cast<Ft8AgentClient*>(client()), &Ft8AgentClient::statusReceived,
            this, &Ft8SpotFeed::onStatus);
}

qint64 Ft8SpotFeed::firstCycle() const
{
    // From now on, not from the receiver's whole history: a receiver that has
    // been up for hours holds hundreds of spots, and replaying them as fresh
    // would put stale decodes on the panadapter at startup.
    return QDateTime::currentSecsSinceEpoch();
}

void Ft8SpotFeed::onStatus(const Ft8Status& status)
{
    if (!isRunning()) {
        return;
    }
    emit statusReceived(status);
    m_receiverCall = status.callsign;
    // Read every poll, not cached: a color or lifetime changed in SpotHub
    // applies from the next poll.
    const WsprSpotHubSettings style = wspr::loadSpotHubSettings(QLatin1String(kFt8SpotHubKey));
    const QList<Ft8Spot> fresh = ft8::spotsSince(status.spots, m_since, &m_since);
    for (const Ft8Spot& s : fresh) {
        emit decoded(s);
        if (s.call.isEmpty()) {
            continue;   // nothing to label a spot with
        }
        emit spotReceived(ft8::styled(ft8::toDxSpot(s, status.callsign), style));
    }
    if (!fresh.isEmpty()) {
        m_recent.append(fresh);
        // An FT8 cycle is 15 s against WSPR's 120 s and a busy band yields
        // ~10 decodes a cycle, so this fills about 8x faster. The receiver
        // itself keeps far fewer.
        constexpr qsizetype kMaxRecent = 2000;
        if (m_recent.size() > kMaxRecent) {
            m_recent.remove(0, m_recent.size() - kMaxRecent);
        }
        emit spotsUpdated();
    }
}

namespace ft8 {

QMetaObject::Connection reserveReceiverSlice(Ft8SpotFeed* feed, RadioModel* radio,
                                             const QString& label)
{
    return AetherSDR::reserveReceiverSlice(static_cast<RadioAgentSpotFeed*>(feed), radio, label);
}

}  // namespace ft8

}  // namespace AetherSDR
