#pragma once

#include "core/hfgateway/HfGatewayConfig.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>
#include <optional>

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

namespace AetherSDR::ModemHost {
class Client;
struct Notification;
enum class Request;
}

namespace AetherSDR::HfGateway {

struct HeardStation {
    QString callsign;
    QString kind;  // CALL or CQ
    int bandwidthHz = 0;
    std::optional<double> snDb;
    QDateTime lastHeardUtc;
    int count = 0;
};

class HfGatewayModemModel : public QObject {
    Q_OBJECT

public:
    static constexpr int kMaxHeard = 50;

    explicit HfGatewayModemModel(const EngineConfig& config, QObject* parent = nullptr);
    ~HfGatewayModemModel() override;

    enum class State { Disconnected, Connecting, Connected };

    static Config loadConfig();
    static bool saveConfig(const Config& config);

    const EngineConfig& config() const { return m_config; }

    // Caller confirms the modem is idle and configured for receive only.
    void connectToModem();
    void disconnectFromModem();
    State state() const;

    // Mercury only: MONITOR on or off. Remembered across reconnects within
    // this engine's life, never stored. Off by default: it costs several
    // times the modem's CPU (Mercury docs/TNC.md, MONITOR).
    bool monitorSupported() const { return m_config.profile == Profile::Mercury; }
    void setMonitor(bool on);
    bool monitor() const { return m_monitor; }
    bool monitorActive() const { return m_monitorActive; }

    QString version() const { return m_version; }
    bool busy() const { return m_busy; }
    bool hasSn() const { return m_hasSn; }
    double snDb() const { return m_snDb; }
    const QList<HeardStation>& heard() const { return m_heard; }
    void clearHeard();

    // Read-only view for tests and diagnostics.
    bool transmitPermitted() const;

signals:
    void changed();
    void heardChanged();
    // A line for the operator's log; `warning` marks things worth noticing.
    void logLine(const QString& text, bool warning);

private:
    void onStateChanged();
    void sendMonitorSetting();
    void monitorOffBeforeDisconnect();
    void onReplied(ModemHost::Request request, bool ok, const QString& version);
    void onNotified(const ModemHost::Notification& n);
    void noteHeard(const QString& call, const QString& kind, int bandwidthHz, std::optional<double> snDb);

    EngineConfig m_config;
    std::unique_ptr<ModemHost::Client> m_client;
    QTimer m_listenOffTimeout;
    bool m_ready = false;
    bool m_monitor = false;
    bool m_monitorActive = false;
    bool m_monitorMayBeActive = false;
    QString m_version;
    bool m_busy = false;
    bool m_hasSn = false;
    double m_snDb = 0.0;
    QList<HeardStation> m_heard;
};

}  // namespace AetherSDR::HfGateway
