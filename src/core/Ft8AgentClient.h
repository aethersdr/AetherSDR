#pragma once

#include "Ft8Agent.h"
#include "RadioAgentClient.h"

#include <QHostAddress>
#include <QObject>
#include <QString>

namespace AetherSDR {

class RadioModel;

// AetherFT8 as a RadioAgent: its port, and a poll every 15 s. An FT8 cycle
// IS 15 s, so a slower poll would show a decode later than the operator hears
// it; each poll asks only for cycles since the last, so the reply stays small,
// and the radio is a 1-core Cortex-A8 already near its decode budget, so this
// deliberately does not go faster than the cycle rate.
inline constexpr RadioAgent kFt8RadioAgent{"FT8", kFt8Port, 15000};

// Talks to the FT8 receiver on the radio (Ft8Agent.h). The request and its
// safety properties are RadioAgentClient's, shared with WsprAgentClient;
// this reads FT8's JSON.
class Ft8AgentClient : public RadioAgentClient {
    Q_OBJECT

public:
    explicit Ft8AgentClient(QObject* parent = nullptr);

    // The first save claims the receiver and yields its admin token
    // (configSaved's `newToken`); later ones need it.
    void saveConfig(const Ft8Config& config, const QString& adminToken);

signals:
    void statusReceived(const AetherSDR::Ft8Status& status);
    void configReceived(const AetherSDR::Ft8Config& config, bool claimed);
    void configSaved(const AetherSDR::Ft8Config& config, const QString& newToken);

protected:
    void received(const QString& operation, const QByteArray& body) override;
};

// Polls the FT8 receiver and turns its decodes into spots. The polling is
// RadioAgentSpotFeed's, shared with WsprSpotFeed; what differs is that it
// starts from now rather than replaying a lifetime of spots, and that an FT8
// decode carries no dB SNR.
class Ft8SpotFeed : public RadioAgentSpotFeed {
    Q_OBJECT

public:
    explicit Ft8SpotFeed(QObject* parent = nullptr);

    const QList<Ft8Spot>& recentSpots() const { return m_recent; }
    QString receiverCallsign() const { return m_receiverCall; }

signals:
    void statusReceived(const AetherSDR::Ft8Status& status);
    // Every new decode, for a console that wants them all.
    void decoded(const AetherSDR::Ft8Spot& spot);

protected:
    qint64 firstCycle() const override;
    void forgetRecent() override { m_recent.clear(); }

private:
    void onStatus(const Ft8Status& status);

    QList<Ft8Spot> m_recent;
    QString m_receiverCall;
};

namespace ft8 {
// Every status the feed reads reserves the slice the receiver names (#7139),
// so a missed read after a new slice heals on the next poll. Same shape as
// wspr::reserveReceiverSlice.
QMetaObject::Connection reserveReceiverSlice(Ft8SpotFeed* feed, RadioModel* radio,
                                             const QString& label);
}  // namespace ft8

}  // namespace AetherSDR
