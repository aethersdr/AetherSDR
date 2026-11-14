#pragma once

#include "core/hfgateway/HfGatewayConfig.h"
#include "core/hfgateway/ModemHostClient.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>

// One configured external modem, as the HF Gateway's Stage 1 page sees it
// (RFC #7296): receive-only. It connects when the operator asks, reports the
// modem's health, and for Mercury can turn on the read-only MONITOR to fill a
// heard list.
//
// Receive-only by construction. The engine never grants its client transmit
// permission and has no API that could, so the client refuses LISTEN ON and
// data writes. On connect it sends LISTEN OFF so the modem can't answer a
// call through this connection. A PTT ON the modem reports anyway, e.g.
// from its own GUI, is surfaced as a warning; it is never acted on.

namespace AetherSDR::HfGateway {

struct HeardStation {
    QString callsign;
    QString kind;  // CALL or CQ
    int bandwidthHz = 0;
    double snDb = 0.0;
    QDateTime lastHeardUtc;
    int count = 0;
};

class HostPortEngine : public QObject {
    Q_OBJECT

public:
    static constexpr int kMaxHeard = 50;

    explicit HostPortEngine(const EngineConfig& config, QObject* parent = nullptr);

    const EngineConfig& config() const { return m_config; }

    void connectToModem();
    void disconnectFromModem();
    ModemHost::Client::State state() const { return m_client.state(); }

    // Mercury only: MONITOR on or off. Remembered across reconnects within
    // this engine's life, never stored. Off by default: it costs several
    // times the modem's CPU (Mercury docs/TNC.md, MONITOR).
    bool monitorSupported() const { return m_config.profile == Profile::Mercury; }
    void setMonitor(bool on);
    bool monitor() const { return m_monitor; }

    QString version() const { return m_version; }
    bool busy() const { return m_busy; }
    bool hasSn() const { return m_hasSn; }
    double snDb() const { return m_snDb; }
    const QList<HeardStation>& heard() const { return m_heard; }
    void clearHeard();

    // Read-only view for tests and diagnostics.
    bool transmitPermitted() const { return m_client.transmitPermitted(); }

signals:
    void changed();
    void heardChanged();
    // A line for the operator's log; `warning` marks things worth noticing.
    void logLine(const QString& text, bool warning);

private:
    void onState(ModemHost::Client::State state);
    void onReplied(ModemHost::Request request, bool ok, const QString& version);
    void onNotified(const ModemHost::Notification& n);
    void noteHeard(const QString& call, const QString& kind, int bandwidthHz, double snDb);

    EngineConfig m_config;
    ModemHost::Client m_client;
    bool m_monitor = false;
    QString m_version;
    bool m_busy = false;
    bool m_hasSn = false;
    double m_snDb = 0.0;
    QList<HeardStation> m_heard;
};

}  // namespace AetherSDR::HfGateway
