#pragma once

#include "core/RadioDiscovery.h"   // RadioInfo

#include <QHash>
#include <QList>
// For QNetworkAddressEntry, broadcastTargets()' parameter. The whole-module
// include rather than the per-class one because this tree already depends on
// it in six places and it is the form every one of them uses.
#include <QNetworkInterface>
#include <QObject>
#include <QString>

#include <array>
#include <cstdint>

class QTimer;
class QUdpSocket;

namespace AetherSDR::anan {

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

    // Begin periodic sweeps. Safe to call twice; restarts the cadence.
    void start(int intervalMs = 5000);
    void stop();
    // Broadcast one discovery datagram now (also called by the interval timer).
    void sweepNow();

    [[nodiscard]] bool isRunning() const noexcept;

    // Every address one sweep sends its discovery datagram to, built from the
    // host's own interface addresses.
    //
    // WHY THIS EXISTS: a datagram to 255.255.255.255 leaves by ONE interface --
    // whichever the host's default route picks -- so on a machine with the radio
    // on a second NIC, a VPN up, or a virtual bridge in front of the default
    // route, the radio never sees it and never appears in the picker. A directed
    // broadcast (the per-subnet x.x.x.255) is routed by its destination instead,
    // so one per interface reaches all of them.
    //
    // 255.255.255.255 IS ALWAYS FIRST IN THE RESULT, and that is the point: the
    // directed addresses are added to what this already did rather than
    // replacing it, so a host where enumeration comes back empty or wrong is no
    // worse off than before. Entries are deduplicated and IPv4-only; loopback
    // and interfaces with no broadcast address of their own are skipped.
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
};

}  // namespace AetherSDR::anan
