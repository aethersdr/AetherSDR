#pragma once

#include "core/backends/hl2/Hl2TelemetryCadence.h"   // Hl2LinkState, hl2PollIntervalMs
#include "core/backends/hl2/MetisProtocol.h"        // DiscoveryReply

#include <array>
#include <cstdint>
#include <optional>

#include <QElapsedTimer>
#include <QHostAddress>
#include <QObject>

class QTimer;
class QUdpSocket;

namespace AetherSDR::hl2 {

// Reads the radio's state without an IQ stream, over alternate control port
// 1025 (docs/architecture/hl2-stream-free-telemetry.md): for when another client
// holds the radio, our stream has stalled, or we are not connected. Port 1025
// because network.v:686-698 updates port 1024's reply destination only when
// ~run, so a 1024 poll during someone else's stream is answered to them.
// Read-only: it sends only the EF FE 02 status request (never start/stop, EF FE
// 05 or a register write), which is what makes polling a busy radio safe.
class Hl2TelemetryPoller : public QObject {
    Q_OBJECT

public:
    // The cadence rule lives in Hl2TelemetryCadence.h.
    using LinkState = Hl2LinkState;

    explicit Hl2TelemetryPoller(QObject* parent = nullptr);
    ~Hl2TelemetryPoller() override;

    // The radio to poll. Required: with no target and no broadcast fallback
    // enabled, nothing is sent. A broadcast reaches only the local segment,
    // which may hold hosts that must not be polled and not the radio at all.
    void setTarget(const QHostAddress& addr);
    // Opt IN to broadcasting when no target is known. Off by default; see
    // setTarget. Only sensible where the radio is known to share a segment with
    // the host AND nothing on that segment minds a discovery datagram.
    void setAllowBroadcastFallback(bool allow);
    void setLinkState(LinkState s);
    // Whether anything is looking at the telemetry. Consulted in NotConnected
    // and HeldByOther (see hl2PollIntervalMs).
    void setSurfaceVisible(bool visible);

    [[nodiscard]] LinkState linkState() const noexcept { return m_state; }
    // Milliseconds between polls for the current state; 0 means "do not poll",
    // including when there is no destination. Both this and onPollTimer() ask
    // pollDestination(), so the diagnostics readout matches the wire.
    [[nodiscard]] int currentIntervalMs() const;

    // The address the last accepted reply came from. Null until one has. Lets a
    // caller learn the radio's address from the poller rather than the other
    // way round.
    [[nodiscard]] QHostAddress lastResponder() const noexcept { return m_lastResponder; }

signals:
    // A reply arrived and parsed. Carries the whole DiscoveryReply because the
    // consumer needs `streaming` alongside the readings: adcClipCount means
    // different things in the two states (see MetisProtocol.cpp), and a surface
    // that shows the number without the state has collapsed them.
    void readingReceived(const AetherSDR::hl2::DiscoveryReply& reply,
                         qint64 ageMs);

    // We asked and nothing came back. Emitted with the count of CONSECUTIVE
    // silent polls, because one lost datagram on a busy LAN is not the same
    // event as a radio that has stopped answering — and "no reading" must not
    // be renderable as "never asked". Three states, not two.
    void pollUnanswered(int consecutive);

private slots:
    void onPollTimer();
    void onReadyRead();

private:
    void applyCadence();
    // Where the next poll would go, or null for "nowhere". The one place this
    // is decided: currentIntervalMs() reports it, onPollTimer() acts on it.
    [[nodiscard]] QHostAddress pollDestination() const;

    // The alternate control port. 1024 + 1: the gateware distinguishes them by
    // the low bit alone (`to_port[0]`, `eth_port[0]`).
    static constexpr std::uint16_t kAltPort = 1025;

    QUdpSocket* m_socket = nullptr;
    QTimer* m_timer = nullptr;
    QHostAddress m_target;          // null = broadcast and take what answers
    QHostAddress m_lastResponder;
    // Set from the first accepted reply and cleared by setTarget(). It does not stop a stranger being believed once;
    // it stops the responder changing underneath a live aim. See onReadyRead().
    std::optional<std::array<std::uint8_t, 6>> m_latchedMac;
    bool m_allowBroadcast = false;   // see setTarget for why this is the default
    LinkState m_state = LinkState::NotConnected;
    bool m_surfaceVisible = false;
    int m_unanswered = 0;
    QElapsedTimer m_sinceRequest;
};

}  // namespace AetherSDR::hl2
