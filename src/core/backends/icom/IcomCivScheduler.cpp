#include "core/backends/icom/IcomCivScheduler.h"

#include <algorithm>
#include <utility>

namespace AetherSDR::icom {

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

    if (request.coalesce) {
        // A duplicate read in flight is already the freshest read for this
        // generation.  Do not accumulate a second copy behind it.
        if (!request.supersedes && m_inFlight
            && m_inFlight->request.key == request.key
            && m_inFlight->generation == currentGeneration) {
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
            const bool sameShape = it->request.expectsReply == request.expectsReply
                && it->request.acceptsGenericReply == request.acceptsGenericReply;
            if (!sameShape) {
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

    if (request.notBeforeMs < nowMs) {
        request.notBeforeMs = nowMs;
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
    m_inFlight.reset();
    ++m_stats.timeouts;
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
        // unanswered transaction.  Forgetting that transaction is safe: any
        // late keyed state is generation-guarded, and the unkey ACK must own
        // the serial reply slot from this point forward.
        if (selected.request.priority == Priority::Emergency) {
            m_inFlight.reset();
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
    // as far as the PTT fallback poll, so a dead or slow radio cannot let the
    // high-rate PTT/S-meter loops permanently starve control reconciliation
    // or startup. An actual PTT write/confirmation remains Operator priority.
    const int base = static_cast<int>(request.request.priority);
    const int floor = static_cast<int>(Priority::Ptt);
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
    if (!m_inFlight || !matches(frame, *m_inFlight)) {
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
