#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/backends/icom/CivCodec.h"

namespace AetherSDR::icom {

// The single ordinary writer for Icom CI-V traffic.
//
// An Icom has no meter stream: meters, front-panel reconciliation, startup
// snapshots and operator commands all share one serial command plane.  Sending
// each producer directly creates bursts, unbounded stale reads and, for PTT, a
// causal inversion where an old RX answer can arrive after a newer TX request.
//
// This class owns policy but not a timer or socket.  The backend supplies a
// monotonic clock, takes dispatches, and writes them through IcomSession.  That
// makes pacing, coalescing and lost/late replies deterministic unit tests.
class IcomCivScheduler {
public:
    enum class Priority : std::uint8_t {
        Emergency = 0,   // fail-safe unkey / teardown; bypasses pacing
        Operator = 1,    // direct operator write and its confirmation
        Ptt = 2,         // radio-authoritative PTT fallback poll
        ActiveMeter = 3, // visible RX/TX instrumentation
        Control = 4,     // periodic front-panel reconciliation
        Maintenance = 5, // startup/scope housekeeping
    };

    struct Request {
        std::vector<std::uint8_t> frame;
        std::string key;
        Priority priority = Priority::Control;
        bool expectsReply = false;
        // Ordinary writes complete with CI-V FB/FA rather than a keyed state
        // frame.  They still occupy the one command/reply slot so their ACK
        // cannot be mistaken for the reply to a later read.
        bool acceptsGenericReply = false;
        std::uint8_t replyCmd = 0;
        bool replyHasSub = false;
        std::uint8_t replySub = 0;
        // A write supersedes all older observations of the same semantic key.
        bool supersedes = false;
        // Reads and repeated slider writes collapse to the newest queued item.
        bool coalesce = true;
        std::int64_t notBeforeMs = 0;
    };

    struct Dispatch {
        std::vector<std::uint8_t> frame;
        std::string key;
        Priority priority = Priority::Control;
        std::uint64_t generation = 0;
        bool supersedes = false;
    };

    enum class Observation : std::uint8_t {
        Unmatched,
        Accepted,
        Stale,
    };

    struct Stats {
        std::uint64_t queued = 0;
        std::uint64_t dispatched = 0;
        std::uint64_t coalesced = 0;
        std::uint64_t replies = 0;
        std::uint64_t staleReplies = 0;
        std::uint64_t timeouts = 0;
        std::size_t queueDepth = 0;
        bool readInFlight = false;
        std::string inFlightKey;
        std::int64_t lastDispatchMs = 0;
    };

    // Returns the semantic generation assigned to the request.  A return of
    // zero means an identical read was already queued/in flight and this one
    // was coalesced away.
    std::uint64_t enqueue(Request request, std::int64_t nowMs);
    [[nodiscard]] std::optional<Dispatch> takeNext(std::int64_t nowMs);
    [[nodiscard]] Observation observe(const CivFrame& frame, std::int64_t nowMs);

    void reset() noexcept;
    [[nodiscard]] Stats stats() const;
    [[nodiscard]] bool idle() const noexcept { return m_queue.empty() && !m_inFlight; }

    static constexpr int kSlotMs = 25;
    static constexpr int kReadTimeoutMs = 350;
    static constexpr int kPriorityAgingMs = 1000;
    // How long a timed-out or displaced transaction stays recognisable, so a
    // late answer is still generation-checked rather than adopted as fresh
    // radio truth. Comfortably longer than kReadTimeoutMs and shorter than the
    // slowest reconciliation interval, so it never spans two generations of
    // the same register.
    static constexpr int kLateReplyGraceMs = 2000;

private:
    struct Queued {
        Request request;
        std::uint64_t generation = 0;
        std::uint64_t sequence = 0;
        std::int64_t enqueuedAtMs = 0;
    };

    struct Expired {
        Queued request;
        std::int64_t forgetAtMs = 0;
    };

    [[nodiscard]] static bool sameReplyShape(const Request& a, const Request& b) noexcept;
    [[nodiscard]] bool matches(const CivFrame& frame, const Queued& request) const noexcept;
    [[nodiscard]] Priority effectivePriority(const Queued& request,
                                             std::int64_t nowMs) const noexcept;
    void expireRead(std::int64_t nowMs);
    void dropStaleExpired(std::int64_t nowMs);

    // Bounded: one ordinary transaction is outstanding at a time and each entry
    // lives only kLateReplyGraceMs, so this cannot grow with queue depth.
    static constexpr std::size_t kMaxExpiredTracked = 16;

    std::deque<Queued> m_queue;
    std::optional<Queued> m_inFlight;
    std::deque<Expired> m_expired;
    std::unordered_map<std::string, std::uint64_t> m_generations;
    std::uint64_t m_sequence = 0;
    std::int64_t m_lastDispatchMs = 0;
    std::int64_t m_inFlightAtMs = 0;
    Stats m_stats;
};

}  // namespace AetherSDR::icom
