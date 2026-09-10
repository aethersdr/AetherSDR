#pragma once

#include "core/RadioDiscovery.h"   // RadioInfo

#include <QHash>
#include <QObject>
#include <QString>

#include <array>
#include <cstdint>

class QTimer;
class QUdpSocket;

namespace AetherSDR::anan {

// openHPSDR Protocol 2 discovery, shaped to feed the same picker as Flex and
// HL2 discovery: it emits RadioInfo with family="anan" so ConnectionPanel's
// existing onRadioDiscovered/onRadioUpdated/onRadioLost slots consume it
// unchanged once wired up (a later commit).
//
// Asynchronous by construction, mirroring Hl2Discovery: broadcast one
// discovery datagram per sweep, collect replies until the next sweep, age
// out radios that stop answering. Filters replies to isSaturn() only --
// this project supports the G2 bring-up radio and no other P2 board (RFC
// §2.11) -- which is a discovery-time PICKER decision, distinct from (and
// not to be confused with) the RFC's caution that board type must not gate
// BACKEND behaviour once connected.
//
// A radio that is streaming to somebody else answers with status byte 0x03;
// that surfaces as RadioInfo::status "In_Use" rather than being hidden, so
// the operator can see the radio exists but is taken -- same as Hl2Discovery.
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

    // Canonical "AA:BB:CC:DD:EE:FF" rendering of a discovery reply's MAC,
    // which IS RadioInfo::serial for this family. Public (promoted from a
    // file-local helper -- this is precisely the scenario the original
    // comment here flagged: a directed unicast probe now needs
    // byte-identical identity to a broadcast sweep). Mirrors
    // Hl2Discovery::macToSerial exactly; they must agree, since the serial
    // is both the auto-reconnect key and the client-side nickname key.
    static QString macToSerial(const std::array<std::uint8_t, 6>& mac);

    // The nickname to show for this radio: the operator's custom name, or
    // `fallback` when none is set. An ANAN-G2 has no on-radio name store,
    // so the custom name is persisted client-side, keyed by serial (the
    // MAC string) -- the same (family, radioId, "Identity") feature
    // document Hl2Discovery::effectiveNickname uses, but with NO
    // legacy-flat-key migration step: this family never had one to
    // migrate from, unlike HL2's pre-RFC-#4603 history.
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
