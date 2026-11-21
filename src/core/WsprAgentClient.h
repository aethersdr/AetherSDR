#pragma once

#include "DxSpot.h"
#include "RadioAgentClient.h"
#include "WsprAgent.h"

#include <QHostAddress>
#include <QObject>
#include <QString>

namespace AetherSDR {

class RadioModel;

// AetherWSPR as a RadioAgent: its port, and a poll every 30 s (a WSPR cycle
// is two minutes).
inline constexpr RadioAgent kWsprRadioAgent{"WSPR", kWsprPort, 30000};

// Talks to the WSPR receiver on the radio (WsprAgent.h). The request and its
// safety properties are RadioAgentClient's; this reads WSPR's JSON.
class WsprAgentClient : public RadioAgentClient {
    Q_OBJECT

public:
    explicit WsprAgentClient(QObject* parent = nullptr);

    // The first save claims the receiver and yields its admin token
    // (configSaved's `newToken`); later ones need it.
    void saveConfig(const WsprConfig& config, const QString& adminToken);

signals:
    void statusReceived(const AetherSDR::WsprStatus& status);
    void configReceived(const AetherSDR::WsprConfig& config, bool claimed);
    void configSaved(const AetherSDR::WsprConfig& config, const QString& newToken);

protected:
    void received(const QString& operation, const QByteArray& body) override;
};

// Reads the receiver's spots for SpotHub and the panadapter: polls its
// status every 30 s and emits each spot it hasn't seen yet, as a DxSpot in
// the WSPR source's color and lifetime. Starts with the spots still within
// that lifetime, so a restart doesn't flood the panadapter with old ones.
class WsprSpotFeed : public RadioAgentSpotFeed {
    Q_OBJECT

public:
    explicit WsprSpotFeed(QObject* parent = nullptr);

    // The decodes kept for AetherMap, oldest first (bounded), and the
    // station that heard them.
    const QList<WsprSpot>& recentSpots() const { return m_recent; }
    QString receiverCallsign() const { return m_receiverCall; }
    QString receiverGrid() const { return m_receiverGrid; }

signals:
    void statusReceived(const AetherSDR::WsprStatus& status);
    // Every new decode, hashed calls included, with the receiving station's
    // grid for its distance: for SpotHub's console.
    void decoded(const AetherSDR::WsprSpot& spot, const QString& receiverGrid);

protected:
    qint64 firstCycle() const override;
    void forgetRecent() override { m_recent.clear(); }

private:
    void onStatus(const WsprStatus& status);

    QList<WsprSpot> m_recent;
    QString m_receiverCall;
    QString m_receiverGrid;
};

namespace wspr {
// `key` is the receiver's own AppSettings key: WSPR's by default, and
// kFt8SpotHubKey for the FT8 receiver, which keeps the same three settings.
WsprSpotHubSettings loadSpotHubSettings(const QString& key = QStringLiteral("WsprSpots"));
void saveSpotHubSettings(const WsprSpotHubSettings& settings,
                         const QString& key = QStringLiteral("WsprSpots"));
// Every status the feed reads reserves the slice the receiver names (#7139),
// so a missed read after a new slice heals on the next poll.
QMetaObject::Connection reserveReceiverSlice(WsprSpotFeed* feed, RadioModel* radio,
                                             const QString& label);
}  // namespace wspr

}  // namespace AetherSDR
