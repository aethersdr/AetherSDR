#include "core/backends/icom/IcomCivScheduler.h"

#include <algorithm>
#include <utility>

namespace AetherSDR::icom {

// Two requests are duplicates only if they ask the SAME register the same way.
//
// The semantic key is deliberately coarse — 04, 06, 26 and the transceive
// forms all key on "mode" — because that coarseness is what makes an operator
// mode write supersede an in-flight mode read of any form.  Coalescing must
// not inherit it: 04 (mode) and 26 (mode + DATA + filter) are different
// registers, and sendConnectReadBurst queues both on purpose so 26 can correct
// 04 when they disagree.  Collapsing them by key alone silently deleted one of
// the two, and which one survived depended on enqueue order.
bool IcomCivScheduler::sameReplyShape(const Request& a, const Request& b) noexcept
{
    return a.expectsReply == b.expectsReply
        && a.acceptsGenericReply == b.acceptsGenericReply
        && a.replyCmd == b.replyCmd
        && a.replyHasSub == b.replyHasSub
        && (!a.replyHasSub || a.replySub == b.replySub);
}

std::uint64_t IcomCivScheduler::enqueue(Request request, std::int64_t nowMs)
{
    if (request.frame.empty() || request.key.empty()) {
        return 0;
    }

    std::uint64_t& currentGeneration = m_generations[request.key];
    if (currentGeneration == 0) {
        currentGeneration = 1;
    }
    if (request.supersedes) {
        ++currentGeneration;
    }

    // CLAMP BEFORE COALESCING, not after.  The loop below compares this
    // against the notBeforeMs of queued entries, which were clamped to a real
    // monotonic timestamp when THEY were enqueued.  Comparing those against an
    // unclamped 0 — the default every periodic producer passes — made the
    // "an equal-or-better one is already queued" test below always false, so a
    // duplicate erased and re-pushed the queued entry instead of collapsing
    // into it.  That reset enqueuedAtMs, and enqueuedAtMs is what
    // effectivePriority() ages on: any group re-queued at or faster than
    // kPriorityAgingMs (onLinkTick re-queues NR/NB/notch every 1000 ms) could
    // never age at all, and an Operator confirmation read was demoted to
    // Control by the next poll tick that touched the same register.
    if (request.notBeforeMs < nowMs) {
        request.notBeforeMs = nowMs;
    }

    if (request.coalesce) {
        // A duplicate read in flight is already the freshest read for this
        // generation.  Do not accumulate a second copy behind it.
        if (!request.supersedes && m_inFlight
            && m_inFlight->request.key == request.key
            && m_inFlight->generation == currentGeneration
            && sameReplyShape(m_inFlight->request, request)) {
            ++m_stats.coalesced;
            return 0;
        }

        for (auto it = m_queue.begin(); it != m_queue.end();) {
            if (it->request.key != request.key) {
                ++it;
                continue;
            }
            // A newer write replaces an older queued write.  A confirmation
            // read must coexist with the write it confirms, so only collapse
            // requests with the same reply-bearing shape.
            if (!sameReplyShape(it->request, request)) {
                ++it;
                continue;
            }
            // A new write generation and its confirmation replace an older
            // queued generation.  This is what makes a fast slider converge
            // on its newest value rather than preserving the first unsent one.
            if (it->generation < currentGeneration) {
                it = m_queue.erase(it);
                ++m_stats.coalesced;
                continue;
            }
            if (it->request.priority <= request.priority
                && it->request.notBeforeMs <= request.notBeforeMs) {
                ++m_stats.coalesced;
                return 0;
            }
            it = m_queue.erase(it);
            ++m_stats.coalesced;
        }
    }

    m_queue.push_back(Queued{std::move(request), currentGeneration, ++m_sequence, nowMs});
    ++m_stats.queued;
    m_stats.queueDepth = m_queue.size();
    return currentGeneration;
}

void IcomCivScheduler::expireRead(std::int64_t nowMs)
{
    if (!m_inFlight) {
        return;
    }
    if (nowMs - m_inFlightAtMs < kReadTimeoutMs) {
        return;
    }
    // A timed-out transaction is no longer in flight, but the radio may still
    // answer it — a late reply is exactly what kReadTimeoutMs exists to stop
    // waiting for, not a promise that it will never arrive.  Remember it so
    // observe() can still recognise the answer and reject it against a newer
    // generation.  Without this the SAME frame was Stale at 349 ms and
    // unmatched-therefore-authoritative at 351 ms, which let an obsolete read
    // overwrite a newer operator write on every register except PTT (which the
    // backend's separate intent window happens to cover).
    m_expired.push_back(Expired{*m_inFlight, nowMs + kLateReplyGraceMs});
    while (m_expired.size() > kMaxExpiredTracked) {
        m_expired.pop_front();
    }
    m_inFlight.reset();
    ++m_stats.timeouts;
}

void IcomCivScheduler::dropStaleExpired(std::int64_t nowMs)
{
    while (!m_expired.empty() && m_expired.front().forgetAtMs <= nowMs) {
        m_expired.pop_front();
    }
}

std::optional<IcomCivScheduler::Dispatch> IcomCivScheduler::takeNext(std::int64_t nowMs)
{
    expireRead(nowMs);
    if (m_queue.empty()) {
        return std::nullopt;
    }

    auto best = m_queue.end();
    for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
        if (it->request.notBeforeMs > nowMs) {
            continue;
        }
        const bool emergency = it->request.priority == Priority::Emergency;
        if (it->request.expectsReply && m_inFlight && !emergency) {
            continue;
        }
        if (!emergency && m_lastDispatchMs > 0 && nowMs - m_lastDispatchMs < kSlotMs) {
            continue;
        }
        const Priority candidatePriority = effectivePriority(*it, nowMs);
        const Priority bestPriority = best == m_queue.end()
            ? Priority::Maintenance : effectivePriority(*best, nowMs);
        if (best == m_queue.end()
            || candidatePriority < bestPriority
            || (candidatePriority == bestPriority
                && it->sequence < best->sequence)) {
            best = it;
        }
    }
    if (best == m_queue.end()) {
        return std::nullopt;
    }

    Queued selected = std::move(*best);
    m_queue.erase(best);
    m_lastDispatchMs = nowMs;
    if (selected.request.expectsReply) {
        // A fail-safe unkey is the sole command allowed to interrupt an
        // unanswered transaction.  Displacing that transaction is safe, but it
        // must not be forgotten outright: the radio can still answer it, and
        // the answer predates the unkey.  Park it with the timed-out ones so
        // observe() keeps generation-guarding it instead of treating a late
        // pre-unkey reading as fresh radio truth.
        if (selected.request.priority == Priority::Emergency && m_inFlight) {
            m_expired.push_back(Expired{*m_inFlight, nowMs + kLateReplyGraceMs});
            while (m_expired.size() > kMaxExpiredTracked) {
                m_expired.pop_front();
            }
        }
        m_inFlight = selected;
        m_inFlightAtMs = nowMs;
    }
    ++m_stats.dispatched;
    m_stats.queueDepth = m_queue.size();
    m_stats.lastDispatchMs = nowMs;
    m_stats.readInFlight = m_inFlight.has_value();
    m_stats.inFlightKey = m_inFlight ? m_inFlight->request.key : std::string{};

    return Dispatch{std::move(selected.request.frame), std::move(selected.request.key),
                    selected.request.priority, selected.generation,
                    selected.request.supersedes};
}

IcomCivScheduler::Priority
IcomCivScheduler::effectivePriority(const Queued& request, std::int64_t nowMs) const noexcept
{
    // Emergency/operator ordering is invariant. Background work ages up only
    // as far as the visible-meter band, so a dead or slow radio cannot let the
    // high-rate PTT/S-meter loops permanently starve control reconciliation
    // or startup. An actual PTT write/confirmation remains Operator priority.
    //
    // THE FLOOR IS ActiveMeter, NOT Ptt.  takeNext() breaks an equal-priority
    // tie on sequence, and an aged item always has the lower sequence — so
    // letting background work reach Priority::Ptt did not merely stop
    // outranking the keyed-state fallback poll, it dispatched ahead of it, one
    // whole poll interval per aged entry.  Aging to ActiveMeter still beats
    // fresh meter traffic on the FIFO tie (which is all anti-starvation needs)
    // while leaving PTT a strict edge that no amount of waiting can erode.
    const int base = static_cast<int>(request.request.priority);
    const int floor = static_cast<int>(Priority::ActiveMeter);
    if (base <= floor) {
        return request.request.priority;
    }
    const std::int64_t waitedMs = std::max<std::int64_t>(0, nowMs - request.enqueuedAtMs);
    const int aged = base - static_cast<int>(waitedMs / kPriorityAgingMs);
    return static_cast<Priority>(std::max(floor, aged));
}

bool IcomCivScheduler::matches(const CivFrame& frame, const Queued& request) const noexcept
{
    // FB/FA is the radio's terminal response even when a queried leaf is not
    // implemented.  With exactly one ordinary command outstanding, it is
    // unambiguously the completion for that transaction.  It releases the
    // slot but carries no state; the backend therefore decodes nothing from
    // it and remains radio-authoritative.
    if (frame.isOk() || frame.isNg()) {
        return true;
    }
    if (request.request.acceptsGenericReply) {
        return false;
    }
    if (frame.cmd != request.request.replyCmd
        || frame.hasSub != request.request.replyHasSub) {
        return false;
    }
    return !frame.hasSub || frame.sub == request.request.replySub;
}

IcomCivScheduler::Observation IcomCivScheduler::observe(const CivFrame& frame,
                                                        std::int64_t nowMs)
{
    expireRead(nowMs);
    dropStaleExpired(nowMs);

    // A generic FB/FA only ever completes the live transaction.  Matching it
    // against a parked one would let one ACK retire two transactions.
    const bool generic = frame.isOk() || frame.isNg();

    if (!m_inFlight || !matches(frame, *m_inFlight)) {
        if (generic) {
            return Observation::Unmatched;
        }
        // Late answer to a transaction we stopped waiting for.  It is only
        // reportable when a newer intent has since superseded it; otherwise it
        // is simply a slow reply and the backend should adopt it as usual.
        for (auto it = m_expired.begin(); it != m_expired.end(); ++it) {
            if (!matches(frame, it->request)) {
                continue;
            }
            const auto generation = m_generations.find(it->request.request.key);
            const bool superseded = generation != m_generations.end()
                && it->request.generation < generation->second;
            m_expired.erase(it);
            if (superseded) {
                ++m_stats.staleReplies;
                return Observation::Stale;
            }
            return Observation::Unmatched;
        }
        return Observation::Unmatched;
    }

    const Queued completed = *m_inFlight;
    m_inFlight.reset();
    ++m_stats.replies;
    m_stats.readInFlight = false;
    m_stats.inFlightKey.clear();

    const auto generation = m_generations.find(completed.request.key);
    if (generation != m_generations.end() && completed.generation < generation->second) {
        ++m_stats.staleReplies;
        return Observation::Stale;
    }
    return Observation::Accepted;
}

void IcomCivScheduler::reset() noexcept
{
    m_queue.clear();
    m_inFlight.reset();
    m_expired.clear();
    m_generations.clear();
    m_sequence = 0;
    m_lastDispatchMs = 0;
    m_inFlightAtMs = 0;
    m_stats = Stats{};
}

IcomCivScheduler::Stats IcomCivScheduler::stats() const
{
    Stats out = m_stats;
    out.queueDepth = m_queue.size();
    out.readInFlight = m_inFlight.has_value();
    out.inFlightKey = m_inFlight ? m_inFlight->request.key : std::string{};
    return out;
}

}  // namespace AetherSDR::icom
