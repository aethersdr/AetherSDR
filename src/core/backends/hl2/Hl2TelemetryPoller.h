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

// Reads the radio's own state WITHOUT an IQ stream, over the alternate control
// port 1025. Roadmap item #15; the design note is
// docs/architecture/hl2-stream-free-telemetry.md.
//
// WHY THIS EXISTS AT ALL. The in-band path already delivers every one of these
// fields at 10 Hz while we hold the stream, and it costs nothing extra because
// the telemetry rides the EP6 C&C bytes. That is precisely its limitation: the
// bytes that carry the radio's state are the bytes that stop arriving when the
// stream is the thing that broke. A transport cannot report its own silence.
// This poller covers the three cases the in-band path structurally cannot —
// another client holds the radio, our own stream has stalled, and we are not
// connected yet.
//
// PORT 1025, NOT 1024, AND NOT AS A PREFERENCE. Both ports answer EF FE 02 with
// the same 60-byte reply and neither is gated on `run` (dsopenhpsdr1.v:185-207).
// What differs is where the answer is addressed. network.v:686-698 keeps two
// destinations and updates the port-1024 one only `else if (~run)`, so a
// port-1024 poll issued while somebody else is streaming is answered TO THAT
// SOMEBODY ELSE and the asker hears nothing. Port 1025 keeps its own
// destination and always answers whoever asked.
//
// READ-ONLY BY CONSTRUCTION. This class sends exactly one packet type, the
// EF FE 02 status request. It never sends metis-start/stop on 1024, never
// issues a port-1025 command (EF FE 05), and never writes a register. That is
// not tidiness — it is what makes it safe to poll a radio another operator is
// using, which is the whole point of the feature.
class Hl2TelemetryPoller : public QObject {
    Q_OBJECT

public:
    // LinkState and the cadence table live in Hl2TelemetryCadence.h so the
    // rule can be tested without a socket or an event loop, and so the poller
    // runs the SAME expression the suite pins rather than a copy of it.
    using LinkState = Hl2LinkState;

    explicit Hl2TelemetryPoller(QObject* parent = nullptr);
    ~Hl2TelemetryPoller() override;

    // The radio to poll.
    //
    // A null address does NOT mean "stop": it means "we do not know which radio
    // yet", and the poller falls back to broadcasting the same EF FE 02 to the
    // subnet, exactly as Hl2Discovery does on 1024. That is the case this
    // feature is for -- the app is not connected, so nothing has told it an
    // address, and the radio it wants to read is the one somebody else is
    // using. Requiring a caller to supply the address first would mean routing
    // discovery results down into the backend purely to enable a feature whose
    // whole point is working when the app is not connected to anything.
    //
    // The cadence rule still decides whether anything is sent at all, so a
    // broadcast only happens when the rule already says to poll.
    void setTarget(const QHostAddress& addr);
    // Restrict replies to one radio, by its MAC.
    //
    // BYTES, not the formatted serial string. Comparing the six bytes the reply
    // actually carries is exact and depends on no shared formatting convention;
    // taking Hl2Discovery::macToSerial's string would make this agree with that
    // function by construction, and drag the whole AppSettings layer into a
    // socket class that has no business knowing about settings.
    //
    // Unset accepts the first HL2 that answers, which is right for a
    // single-radio bench and wrong the moment there are two -- so a caller that
    // knows which radio it means should say so.
    void setExpectedMac(const std::array<std::uint8_t, 6>& mac);
    void setLinkState(LinkState s);
    // Whether anything is actually looking at the telemetry. Only consulted in
    // NotConnected: polling a radio nobody is watching is pure wire cost.
    void setSurfaceVisible(bool visible);

    [[nodiscard]] LinkState linkState() const noexcept { return m_state; }
    // Milliseconds between polls for the current state; 0 means "do not poll".
    // Public so a diagnostics surface can show the operator what it is doing
    // rather than leaving the cadence invisible.
    [[nodiscard]] int currentIntervalMs() const noexcept;

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

    // The alternate control port. 1024 + 1: the gateware distinguishes them by
    // the low bit alone (`to_port[0]`, `eth_port[0]`).
    static constexpr std::uint16_t kAltPort = 1025;

    QUdpSocket* m_socket = nullptr;
    QTimer* m_timer = nullptr;
    QHostAddress m_target;          // null = broadcast and take what answers
    QHostAddress m_lastResponder;
    std::optional<std::array<std::uint8_t, 6>> m_expectedMac;
    LinkState m_state = LinkState::NotConnected;
    bool m_surfaceVisible = false;
    int m_unanswered = 0;
    QElapsedTimer m_sinceRequest;
};

}  // namespace AetherSDR::hl2
