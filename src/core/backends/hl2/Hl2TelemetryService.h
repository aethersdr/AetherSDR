#pragma once

// Stream-free HL2 telemetry, owned ABOVE the backend (roadmap #15).
//
// THE RULE THIS EXISTS TO OBEY: an instrument for the no-connection case must
// not be owned by the connection.
//
// The poller first lived inside Hl2Backend, which was wrong in a way every test
// passed through. `RadioModel::backendHealthSnapshot()` is
// `m_backend ? m_backend->healthSnapshot() : HealthSnapshot{}`, and m_backend is
// constructed inside connectToRadio(). So with the app not connected there was
// no backend, therefore no poller, therefore an EMPTY health snapshot — in
// precisely the state the feature was built for: another client holding the
// radio, or nothing connected yet. Measured, not reasoned: two prechecks
// against a disconnected app returned `total rows in snapshot: 0`.
//
// The unit tests were green throughout, the symbols were verifiably linked into
// the binary, and none of that asks whether anything CONSTRUCTS the thing in
// the state that matters.
//
// So this object's lifetime is the application's, not a connection's. It owns
// the poller, it answers with health rows whether or not a backend exists, and
// the backend — when there is one — supplies the in-band rows that take
// precedence over these at the merge point.

#include "core/backends/IRadioBackend.h"        // HealthSnapshot
#include "core/backends/hl2/Hl2TelemetryCadence.h"
#include "core/backends/hl2/MetisProtocol.h"    // DiscoveryReply

#include <QElapsedTimer>
#include <QHostAddress>
#include <QObject>
#include <QString>

namespace AetherSDR::hl2 {

class Hl2TelemetryPoller;

class Hl2TelemetryService : public QObject {
    Q_OBJECT

public:
    explicit Hl2TelemetryService(QObject* parent = nullptr);
    ~Hl2TelemetryService() override;

    // The radio to read. A null address means "we do not know one yet", and the
    // poller broadcasts rather than stopping — see Hl2TelemetryPoller.
    void setTarget(const QHostAddress& addr);
    void setExpectedMac(const std::array<std::uint8_t, 6>& mac);
    // Opt in to broadcasting when no target is set. OFF by default -- a
    // broadcast reaches the local segment, which on this bench is not where
    // the radio is and is where the station receiver is.
    void setAllowBroadcastFallback(bool allow);

    // What the IQ path is doing. Driven by whoever knows: the backend while one
    // exists, the model's connection state otherwise. The cadence rule turns
    // this into an interval; this class does not restate it.
    void setLinkState(Hl2LinkState state);
    // How many times a link state has been pushed in. Exists so a test can ask
    // the question that matters and cannot be asked any other way: is anything
    // DRIVING this periodically?
    //
    // The tick that drives it was deleted once already, by a refactor whose
    // regex swallowed it along with the code beside it. Nothing noticed: the
    // cadence rule was still correct, its unit test still passed, and the
    // poller simply kept polling through a live stream because nobody ever
    // told it one had started. A counter is a cheap thing to expose; a rule
    // nobody asks is an expensive thing to ship.
    [[nodiscard]] int linkStateUpdateCount() const noexcept;

    // Reading the health snapshot IS the demand signal — it is the one thing
    // every consumer does, so no consumer can forget to announce itself. Call
    // from the health path, not from a UI show/hide, or the bridge and the
    // dialog disagree about whether anyone is watching.
    void noteDemand();

    // Stream-free rows for the health snapshot. Always answers, backend or no
    // backend; that is the entire point of the class.
    [[nodiscard]] IRadioBackend::HealthSnapshot healthRows() const;

    // The newest stream-free reply, for the backend's in-band merge. nullopt
    // until one has arrived — never a default-constructed reply, because "the
    // radio has not answered" and "the radio answered with zeros" are different
    // claims and only one is a measurement.
    [[nodiscard]] std::optional<DiscoveryReply> lastReply() const;

private:
    struct Impl;
    Impl* d = nullptr;
};

}  // namespace AetherSDR::hl2
