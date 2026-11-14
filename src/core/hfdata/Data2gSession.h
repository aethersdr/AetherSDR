#pragma once

// Data2G connected-mode session (RFC #7159, Stage 2).
//
// Runs the vendored Data2G ARQ engine for one operator-initiated session:
// receive audio in, and out come the audio the engine wants on the air, a
// request to key for it, session events and received bytes.
//
// This class has no radio. It cannot key anything: a transmission it hands
// back is a request, and whether it goes on the air belongs to the caller,
// behind TX admission. What it does guarantee is the first half of the RFC's
// contract:
//
//  - It never listens. An incoming CONNECT is never answered with a session.
//  - It hands back a transmission only for a session the operator started
//    with requestConnect(), and for that session's closing identification.
//  - A refusal is never handed back. The engine answers a CONNECT it will not
//    take with a CONNECT_NAK, whether idle or busy in a session; every one is
//    dropped here and counted. An unarmed station stays silent, and an armed
//    one transmits only to its own peer.
//
// Two costs of dropping a refusal, both the engine's doing. It still plays the
// refusal out on its own clock and does not listen meanwhile, so a third
// station calling mid-session costs the session a few seconds of reception.
// And when an identification falls due in the same turn, the engine sends the
// two as one transmission and counts itself as having identified: that
// identification is dropped with the refusal, and the next comes a full
// interval (600 s) later.
//
// Qt-free, and the Data2G headers stay out of this header, for the reasons
// given in Data2gMonitor.h.
//
// Threading: step() belongs to one thread and can block for a noticeable
// fraction of a second on a long decode, so it runs on a worker. The request
// functions may be called from any thread; each takes effect at the start of
// the next step().

#include <cstdint>
#include <memory>
#include <span>   // std::span in step()
#include <string>
#include <vector>

namespace AetherSDR::hfdata {

enum class Data2gLinkState { Idle, Connecting, Connected, Disconnecting };

// The widest mode the session may use.
enum class Data2gBandwidth { Hz500, Hz1200, Hz2400 };

struct Data2gSessionStep {
    // Set on the step a transmission starts: the whole transmission, at
    // sampleRate(), its envelope peaking at 1.0. It begins with keyLead
    // seconds of silence, so key the transmitter and play it from its first
    // sample. Empty on every other step.
    std::vector<float> transmission;
    // The engine is playing out, on its own clock, a transmission that was
    // handed back. It keeps time by the samples it is fed, so this follows
    // the fed audio, not the radio. False for a transmission dropped here.
    bool transmitting{false};
    // The session was aborted since the last step: stop whatever is playing
    // and unkey now.
    bool abortTx{false};
    Data2gLinkState state{Data2gLinkState::Idle};
    // As the engine words them: "CONNECTED W1AW", "DISCONNECTED TIMEOUT", ...
    // A refused request adds "REFUSED <why>".
    std::vector<std::string> events;
    // Bytes the peer sent, in order.
    std::vector<std::uint8_t> received;
};

class Data2gSession {
public:
    // How long after a session ends its closing identification may still be
    // handed back. Past it, by the engine's clock, it is dropped (maintainer
    // ruling, 2026-11-14).
    static constexpr double kClosingIdSeconds = 6.0;
    // Silence at the head of every transmission, for the transmitter to key.
    static constexpr double kDefaultKeyLeadSeconds = 0.1;

    // An unsupported rate or a call sign the engine cannot pack leaves the
    // session invalid: step() then returns silence and refuses everything.
    explicit Data2gSession(std::string myCall, int sampleRate = 24000);
    // For tests: the same, with the engine's random draws seeded.
    Data2gSession(std::string myCall, int sampleRate, std::uint64_t seed);
    ~Data2gSession();
    Data2gSession(const Data2gSession&) = delete;
    Data2gSession& operator=(const Data2gSession&) = delete;

    bool isValid() const;
    int sampleRate() const;

    // Any thread. Applied at the start of the next step().
    void requestConnect(std::string peerCall, Data2gBandwidth bandwidth);
    void requestDisconnect();   // graceful: the peer is told, queued data still goes
    void requestAbort();        // now: nothing more is sent
    void send(std::vector<std::uint8_t> bytes);
    // From the next transmission keyed. Clamped to 0..1 s.
    void setKeyLeadSeconds(double seconds);

    // The worker thread. Mono float32 at sampleRate(), in chunks of any size.
    // The engine keeps time by sample count, so it must be fed without gaps,
    // transmit included: if the receive tap stops delivering while the radio
    // is keyed, the caller feeds silence at the same rate in its place.
    Data2gSessionStep step(std::span<const float> mono);

    // Worker thread, after a step().
    // Transmissions dropped: refusals, and anything outside an operator
    // session.
    std::uint64_t suppressedTransmissions() const;
    // step() calls that ended in a core exception. Each one aborted the
    // session.
    std::uint64_t faultCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace AetherSDR::hfdata
