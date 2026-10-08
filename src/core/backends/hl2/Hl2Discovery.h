#pragma once

#include "core/RadioDiscovery.h"   // RadioInfo

#include <array>
#include <cstdint>

#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

class QTimer;
class QUdpSocket;

namespace AetherSDR::hl2 {

// Decides which discovery failures reach the log (category aether.discovery).
// Sweeps repeat every few seconds, so each condition is logged once when it
// starts and again only after it has cleared and come back. Socket-free, so
// the policy is testable without a network (tests/hl2_discovery_logging_test).
class DiscoveryNotices {
public:
    // 3 sweeps at Hl2Discovery's default 5000 ms interval: 15 s of silence.
    static constexpr int kSilentSweepsBeforeWarning = 3;

    void bindFailed(const QString& interfaceName, const QHostAddress& address,
                    const QString& error);
    // A logged failure logs again only after this pair has bound once.
    void bindSucceeded(const QString& interfaceName, const QHostAddress& address);
    void endRefresh(int socketCount, int interfaceCount);
    // A sweep's interval ended; `answered` is true if any HL2 replied to it.
    void sweepClosed(bool answered);
    void reset();

private:
    QSet<QString> m_bindFailures;       // failures logged and not bound since
    int m_interfaceCount = 0;
    int m_silentSweeps = 0;
    bool m_noSocketReported = false;
};

// HPSDR Protocol 1 ("Metis") discovery, shaped to feed the same picker as Flex
// discovery: it emits RadioInfo with family="hl2" so ConnectionPanel's existing
// onRadioDiscovered/onRadioUpdated/onRadioLost slots consume it unchanged.
//
// Asynchronous (MetisClient::discover() blocks): one broadcast per sweep,
// replies collected off readyRead, silent radios aged out. Status byte 0x03
// (streaming to another host) surfaces as RadioInfo::status "In_Use".
class Hl2Discovery : public QObject {
    Q_OBJECT

public:
    explicit Hl2Discovery(QObject* parent = nullptr);
    ~Hl2Discovery() override;

    // Begin periodic sweeps. Safe to call twice; restarts the cadence.
    void start(int intervalMs = 5000);
    void stop();
    // Broadcast one discovery datagram now (also called by the interval timer).
    void sweepNow();

    [[nodiscard]] bool isRunning() const noexcept;

    // Canonical "AA:BB:CC:DD:EE:FF" rendering of a discovery reply's MAC, which
    // IS RadioInfo::serial for this family. Shared so a directed (unicast)
    // probe — ConnectionPanel's connect-by-IP path — produces byte-identical
    // identity to a broadcast sweep. They must agree: the serial is the
    // auto-reconnect key and the client-side nickname key.
    static QString macToSerial(const std::array<std::uint8_t, 6>& mac);

    // AppSettings key for a user-assigned nickname for the HL2 with this serial
    // (the MAC string). An HL2 has no on-radio name store (unlike Flex's
    // "radio name" command), so the operator's custom name is persisted
    // client-side, keyed by the radio's stable MAC, and read back here at
    // discovery time. Shared with RadioSetupDialog so both sides agree on the key.
    // LEGACY key builder — kept only for the one-shot migration below and its
    // regression test. New storage is the (family, serial, "Identity") feature
    // document in radio_settings (RFC #4603 PR 3), where the raw MAC is a
    // legal identity and no sanitizing is needed.
    static QString nicknameSettingsKey(const QString& serial);
    // The nickname to show for this radio: the saved custom name, or a default
    // when none is set. Centralises the "custom or fall back" rule, and
    // performs the one-shot legacy-flat-key -> feature-document migration.
    // `family` is the radio's family string (empty is normalized to "hl2",
    // the only family that ever produced legacy keys).
    static QString effectiveNickname(const QString& family, const QString& serial,
                                     const QString& fallback);
    // Store (or clear, with an empty name) the client-side nickname.
    static void setNickname(const QString& family, const QString& serial,
                            const QString& name);
    static bool hasCustomNickname(const QString& family, const QString& serial);
    // True when this radio stores its own name (FlexRadio's "radio name"
    // command), so the client must NOT keep a second copy. False for every
    // family without an on-radio store — HL2, the sim, any future backend —
    // which is exactly the set that persists client-side under the key above.
    // Every site that decides "on-radio or client-side?" must call this: the
    // connect-list menu, RadioSetup's read-back, and RadioSetup's write all
    // have to agree, or a name gets saved somewhere it's never read from.
    static bool nicknameLivesOnRadio(const RadioInfo& info);

signals:
    void radioDiscovered(const RadioInfo& info);
    void radioUpdated(const RadioInfo& info);
    void radioLost(const QString& serial);

private slots:
    void onReadyRead();
    void onSweepTimer();

private:
    void refreshSockets();

    // Radios not seen for this many consecutive sweeps are reported lost. Two
    // sweeps of slack absorbs a single dropped reply on a busy LAN.
    static constexpr int kMissedSweepsBeforeLost = 3;

    struct Seen {
        RadioInfo info;
        int missedSweeps = 0;
    };

    // One socket per eligible local IPv4 address. Binding the discovery socket
    // to the interface is important on multi-homed Windows hosts: a wildcard
    // bind lets the route table choose an interface, which can leave a
    // link-local HL2 invisible until another client has discovered it first.
    QList<QUdpSocket*> m_sockets;
    QTimer* m_timer = nullptr;
    QHash<QString, Seen> m_seen;   // keyed by serial (the MAC string)
    DiscoveryNotices m_notices;
    bool m_probeSent = false;      // the last sweep had a socket to send on
    bool m_answered = false;       // an HL2 replied since the last sweep
};

}  // namespace AetherSDR::hl2
