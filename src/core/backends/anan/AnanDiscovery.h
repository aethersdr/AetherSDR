#pragma once

#include "core/RadioDiscovery.h"   // RadioInfo

#include <QHash>
#include <QHostAddress>
#include <QList>
// For QNetworkAddressEntry, broadcastTargets()' parameter. The whole-module
// include rather than the per-class one because this tree already depends on
// it in six places and it is the form every one of them uses.
#include <QNetworkInterface>
#include <QObject>
#include <QSet>
#include <QString>

#include <array>
#include <cstdint>

class QTimer;
class QUdpSocket;

namespace AetherSDR::anan {

// Decides which discovery problems reach the log (category aether.discovery).
// Sweeps repeat every few seconds, so each condition is logged once when it
// starts and again only after it has cleared and come back. AnanDiscovery
// reports every bind, target list, send, listed reply and closed sweep here;
// socket-free, so tests/anan_discovery_test drives it without a network.
class AnanDiscoveryLog {
public:
    // 3 unanswered sweeps that sent a probe: at AnanDiscovery's default 5000 ms
    // interval, at least 15 s of silence. A sweep no probe left adds time, not
    // a count.
    static constexpr int kSilentSweepsBeforeNotice = 3;

    // The wildcard socket did not bind; the next sweep tries again.
    void bindFailed(const QString& error);
    // A logged bind failure logs again only after a bind has succeeded.
    void bindSucceeded();
    // One sweep's targets, as broadcastTargets() returned them. The global
    // broadcast alone means no interface offered a subnet to probe.
    void targetsListed(const QList<QHostAddress>& targets);
    // No datagram left for `target`. Logged once, and again only after a
    // probe has left for it.
    void sendFailed(const QHostAddress& target, const QString& error);
    // A probe left for `target`: the open sweep can count as unanswered.
    void sendSucceeded(const QHostAddress& target);
    // An ANAN-G2 reply was listed in the picker during the open sweep.
    void replyListed();
    // The open sweep's reply window ended. A sweep no probe left is not
    // silence: bindFailed or sendFailed has named its cause. The silence line
    // counts the targets a probe left in the sweep it closes.
    void sweepClosed();
    void reset();

private:
    QSet<QString> m_sendFailures;   // targets logged and not sent to since
    QSet<QString> m_sentTargets;    // targets a probe left in the open sweep
    int m_silentSweeps = 0;
    bool m_answered = false;        // a reply was listed in the open sweep
    bool m_bindFailureReported = false;
    bool m_noSubnetReported = false;
};

// openHPSDR Protocol 2 discovery. Emits RadioInfo with family="anan" for the
// shared radio picker; sweeps periodically and ages out radios that stop
// answering, like Hl2Discovery. Only isSaturn() replies are listed (RFC §2.11):
// a picker decision only -- board type must not gate backend behaviour once
// connected. Status byte 0x03 (streaming to another client) surfaces as "In_Use".
class AnanDiscovery : public QObject {
    Q_OBJECT

public:
    explicit AnanDiscovery(QObject* parent = nullptr);
    ~AnanDiscovery() override;

    // Begin periodic sweeps. Safe to call twice; restarts the cadence. The
    // sweeps run even when the socket cannot bind: each one tries again.
    void start(int intervalMs = 5000);
    void stop();
    // Broadcast one discovery datagram now (also called by the interval timer).
    // Binds the socket first when there is none. Does nothing unless started:
    // a stopped discovery opens no socket and sends nothing.
    void sweepNow();

    [[nodiscard]] bool isRunning() const noexcept;

    // Every address one sweep sends its discovery datagram to, built from the
    // host's own interface addresses.
    //
    // Directed broadcasts reach a second NIC on a distinct subnet even when
    // the global broadcast follows a different route. Targets are per subnet,
    // not per interface: two NICs sharing a subnet still use the preferred route.
    //
    // 255.255.255.255 IS ALWAYS FIRST IN THE RESULT, and that is the point: the
    // directed addresses are added to what this already did rather than
    // replacing it, so a host where enumeration comes back empty or wrong is no
    // worse off than before. Entries are deduplicated and IPv4-only; loopback
    // and entries without a broadcast domain (/31, /32 or no address) are skipped.
    //
    // Static and taking its input so it can be tested without a host that has
    // the interesting network shape -- which is most hosts.
    [[nodiscard]] static QList<QHostAddress> broadcastTargets(
        const QList<QNetworkAddressEntry>& entries);

    // Canonical "AA:BB:CC:DD:EE:FF" rendering of a discovery reply's MAC, which IS
    // RadioInfo::serial for this family. Must match Hl2Discovery::macToSerial and be
    // byte-identical between broadcast sweeps and directed probes: the serial is the
    // auto-reconnect key and the nickname key.
    static QString macToSerial(const std::array<std::uint8_t, 6>& mac);

    // The operator's custom name, or `fallback`. ANAN-G2 has no on-radio name
    // store, so the name is persisted client-side by serial in the same
    // (family, radioId, "Identity") document Hl2Discovery uses (no legacy-key
    // migration for this family).
    static QString effectiveNickname(const QString& family, const QString& serial,
                                     const QString& fallback);
    // Store (or clear, with an empty name) the client-side nickname.
    static void setNickname(const QString& family, const QString& serial,
                            const QString& name);

signals:
    void radioDiscovered(const RadioInfo& info);
    void radioUpdated(const RadioInfo& info);
    void radioLost(const QString& serial);

private slots:
    void onReadyRead();
    void onSweepTimer();

private:
    friend class AnanDiscoveryTestAccess;

    // Binds the wildcard socket and wires it; false, with the failure
    // reported, when the bind fails.
    bool openSocket();

    // Radios not seen for this many consecutive sweeps are reported lost.
    // Same slack Hl2Discovery uses, for the same reason: absorb one dropped
    // reply on a busy LAN without flapping the picker.
    static constexpr int kMissedSweepsBeforeLost = 3;

    struct Seen {
        RadioInfo info;
        int missedSweeps = 0;
    };

    QUdpSocket* m_socket = nullptr;
    QTimer* m_timer = nullptr;
    QHash<QString, Seen> m_seen;   // keyed by serial (the MAC string)
    AnanDiscoveryLog m_log;
};

}  // namespace AetherSDR::anan
