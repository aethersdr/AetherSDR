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
