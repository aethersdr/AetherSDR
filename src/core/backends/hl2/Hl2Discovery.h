#pragma once

#include "core/RadioDiscovery.h"   // RadioInfo

#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QString>

class QTimer;
class QUdpSocket;

namespace AetherSDR::hl2 {

// HPSDR Protocol 1 ("Metis") discovery, shaped to feed the same picker as Flex
// discovery: it emits RadioInfo with family="hl2" so ConnectionPanel's existing
// onRadioDiscovered/onRadioUpdated/onRadioLost slots consume it unchanged.
//
// Asynchronous by construction. MetisClient::discover() blocks for its whole
// timeout, which is fine for a one-shot probe but would stall the UI on a
// periodic sweep, so this class drives the same exchange off the socket's
// readyRead signal instead: broadcast one discovery datagram per sweep, collect
// replies until the next sweep, and age out radios that stop answering.
//
// A radio that is streaming to somebody else answers with status byte 0x03; that
// surfaces as RadioInfo::status "In_Use" rather than being hidden, so the
// operator can see the radio exists but is taken.
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

    // AppSettings key for a user-assigned nickname for the HL2 with this serial
    // (the MAC string). An HL2 has no on-radio name store (unlike Flex's
    // "radio name" command), so the operator's custom name is persisted
    // client-side, keyed by the radio's stable MAC, and read back here at
    // discovery time. Shared with RadioSetupDialog so both sides agree on the key.
    static QString nicknameSettingsKey(const QString& serial);
    // The nickname to show for this serial: the saved custom name, or a default
    // when none is set. Centralises the "custom or fall back" rule.
    static QString effectiveNickname(const QString& serial,
                                     const QString& fallback);
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
    // Radios not seen for this many consecutive sweeps are reported lost. Two
    // sweeps of slack absorbs a single dropped reply on a busy LAN.
    static constexpr int kMissedSweepsBeforeLost = 3;

    struct Seen {
        RadioInfo info;
        int missedSweeps = 0;
    };

    QUdpSocket* m_socket = nullptr;
    QTimer* m_timer = nullptr;
    QHash<QString, Seen> m_seen;   // keyed by serial (the MAC string)
};

}  // namespace AetherSDR::hl2
