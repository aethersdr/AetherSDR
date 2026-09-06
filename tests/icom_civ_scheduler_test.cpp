#include "core/backends/icom/IcomCivScheduler.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

using namespace AetherSDR::icom;

namespace {
int g_failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

IcomCivScheduler::Request read(std::string key, std::uint8_t cmd, std::uint8_t sub,
                               IcomCivScheduler::Priority priority)
{
    IcomCivScheduler::Request request;
    request.frame = buildFrameSub(0xB6, cmd, sub);
    request.key = std::move(key);
    request.priority = priority;
    request.expectsReply = true;
    request.replyCmd = cmd;
    request.replyHasSub = true;
    request.replySub = sub;
    return request;
}

IcomCivScheduler::Request write(std::string key, std::uint8_t cmd, std::uint8_t sub,
                                std::uint8_t value,
                                IcomCivScheduler::Priority priority)
{
    IcomCivScheduler::Request request;
    request.frame = buildFrameSub(0xB6, cmd, sub, std::array{value});
    request.key = std::move(key);
    request.priority = priority;
    request.expectsReply = true;
    request.acceptsGenericReply = true;
    request.supersedes = true;
    return request;
}

CivFrame reply(std::uint8_t cmd, std::uint8_t sub, std::uint8_t value)
{
    return CivFrame{0xE0, 0xB6, cmd, true, sub, {value}};
}

CivFrame ok()
{
    return CivFrame{0xE0, 0xB6, kCivOk, false, 0, {}};
}
}  // namespace

int main()
{
    using Priority = IcomCivScheduler::Priority;

    {
        IcomCivScheduler scheduler;
        scheduler.enqueue(read("memory.1.1", 0x1A, 0x00, Priority::Maintenance), 0);
        scheduler.enqueue(read("meter.s", 0x15, 0x02, Priority::ActiveMeter), 0);
        check(scheduler.hasPendingKeyPrefix("memory.", 0),
              "memory drain detection sees queued snapshot work");
        check(!scheduler.hasPendingKeyPrefix("scope.", 0),
              "memory drain detection does not match unrelated work");
    }

    {
        IcomCivScheduler scheduler;
        auto first = read("memory.1.1", 0x1A, 0x00, Priority::Maintenance);
        first.replyDataPrefix = {0x01, 0x00, 0x01};
        auto second = read("memory.1.2", 0x1A, 0x00, Priority::Maintenance);
        second.replyDataPrefix = {0x01, 0x00, 0x02};
        scheduler.enqueue(std::move(first), 0);
        scheduler.enqueue(std::move(second), 0);
        check(scheduler.takeNext(0).has_value(), "first memory read dispatches");
        check(scheduler.observe(CivFrame{0xE0, 0xA2, 0x1A, true, 0x00,
                                         {0x01, 0x00, 0x02, 0xFF}}, 10)
                  == IcomCivScheduler::Observation::Unmatched,
              "a different memory slot cannot retire the in-flight read");
        check(scheduler.observe(CivFrame{0xE0, 0xA2, 0x1A, true, 0x00,
                                         {0x01, 0x00, 0x01, 0xFF}}, 20)
                  == IcomCivScheduler::Observation::Accepted,
              "the addressed memory reply completes its own read");
    }

    {
        IcomCivScheduler scheduler;
        scheduler.enqueue(read("meter.s", 0x15, 0x02, Priority::ActiveMeter), 1000);
        scheduler.enqueue(read("meter.s", 0x15, 0x02, Priority::ActiveMeter), 1000);
        check(scheduler.stats().queueDepth == 1, "duplicate reads coalesce");
        const auto first = scheduler.takeNext(1000);
        check(first && first->key == "meter.s", "first read dispatches");
        check(!scheduler.takeNext(1100), "only one reply-bearing read is outstanding");
        check(scheduler.observe(reply(0x15, 0x02, 0), 1110)
                  == IcomCivScheduler::Observation::Accepted,
              "matching reply completes the outstanding read");
        const auto& history = scheduler.recentTransactions();
        check(history.size() == 1 && history.back().key == "meter.s"
                  && history.back().completion == IcomCivScheduler::Completion::Reply
                  && history.back().queueWaitMs == 0
                  && history.back().responseMs == 110,
              "completed reads retain payload-free queue and response timing");
        check(scheduler.stats().responseSamples == 1
                  && scheduler.stats().lastResponseMs == 110
                  && scheduler.stats().maxResponseMs == 110,
              "reply timing contributes to bounded scheduler aggregates");
    }

    {
        IcomCivScheduler scheduler;
        scheduler.enqueue(read("ptt", 0x1C, 0x00, Priority::Ptt), 2000);
        check(scheduler.takeNext(2000).has_value(), "PTT fallback poll dispatches");

        scheduler.enqueue(write("ptt", 0x1C, 0x00, 1, Priority::Operator), 2005);
        const auto operatorWrite = scheduler.takeNext(2030);
        check(!operatorWrite, "operator write waits for the outstanding reply slot");

        scheduler.enqueue(read("ptt", 0x1C, 0x00, Priority::Operator), 2005);
        check(scheduler.observe(reply(0x1C, 0x00, 0), 2040)
                  == IcomCivScheduler::Observation::Stale,
              "old PTT OFF completion is rejected after newer PTT ON intent");
        const auto writeDispatch = scheduler.takeNext(2065);
        check(writeDispatch && writeDispatch->priority == Priority::Operator,
              "operator write runs as soon as the old reply is consumed");
        check(scheduler.observe(ok(), 2070) == IcomCivScheduler::Observation::Accepted,
              "generic write ACK releases the serial reply slot");
        const auto confirmation = scheduler.takeNext(2090);
        check(confirmation && confirmation->key == "ptt",
              "fresh PTT confirmation follows the stale completion");
        check(scheduler.observe(reply(0x1C, 0x00, 1), 2100)
                  == IcomCivScheduler::Observation::Accepted,
              "fresh PTT ON confirmation is accepted");
    }

    {
        IcomCivScheduler scheduler;
        scheduler.enqueue(read("control.nr", 0x16, 0x40, Priority::Control), 3000);
        scheduler.enqueue(read("meter.s", 0x15, 0x02, Priority::ActiveMeter), 3000);
        scheduler.enqueue(write("ptt", 0x1C, 0x00, 0, Priority::Emergency), 3000);
        const auto emergency = scheduler.takeNext(3000);
        check(emergency && emergency->priority == Priority::Emergency,
              "fail-safe unkey wins over every meter/control request");
        check(scheduler.observe(ok(), 3010) == IcomCivScheduler::Observation::Accepted,
              "fail-safe unkey ACK releases the reply slot");
        const auto meter = scheduler.takeNext(3030);
        check(meter && meter->key == "meter.s", "active meter precedes background control");
    }

    {
        IcomCivScheduler scheduler;
        scheduler.enqueue(read("control.nr", 0x16, 0x40, Priority::Control), 4000);
        check(scheduler.takeNext(4000).has_value(), "lost-reply fixture dispatches");
        scheduler.enqueue(read("meter.s", 0x15, 0x02, Priority::ActiveMeter), 4010);
        check(!scheduler.takeNext(4349), "read remains bounded until timeout");
        const auto recovered = scheduler.takeNext(4350);
        check(recovered && recovered->key == "meter.s", "scheduler recovers after lost reply");
        check(scheduler.stats().timeouts == 1, "lost reply is counted");
        const auto& history = scheduler.recentTransactions();
        check(!history.empty() && history.front().key == "control.nr"
                  && history.front().completion == IcomCivScheduler::Completion::Timeout
                  && history.front().responseMs == IcomCivScheduler::kReadTimeoutMs,
              "a lost reply leaves a timed transaction lifecycle record");
    }

    {
        IcomCivScheduler scheduler;
        scheduler.enqueue(read("control.nr", 0x16, 0x40, Priority::Control), 5000);
        scheduler.enqueue(read("meter.s", 0x15, 0x02, Priority::ActiveMeter), 5000);
        const auto meter = scheduler.takeNext(5000);
        check(meter && meter->key == "meter.s", "fresh active meter wins initially");
        check(scheduler.observe(reply(0x15, 0x02, 0), 5010)
                  == IcomCivScheduler::Observation::Accepted,
              "meter completion precedes the aging fixture");
        scheduler.enqueue(read("meter.power", 0x15, 0x11, Priority::ActiveMeter), 7050);
        const auto agedControl = scheduler.takeNext(7050);
        check(agedControl && agedControl->key == "control.nr",
              "aged control reconciliation cannot be starved by fresh meters");
    }

    {
        IcomCivScheduler scheduler;
        scheduler.enqueue(write("level.rf", 0x14, 0x02, 10, Priority::Operator), 7000);
        scheduler.enqueue(write("level.rf", 0x14, 0x02, 20, Priority::Operator), 7001);
        check(scheduler.stats().queueDepth == 1, "rapid writes coalesce to one transaction");
        const auto latest = scheduler.takeNext(7001);
        const auto parsed = latest ? parseFrame(latest->frame) : std::nullopt;
        check(parsed && !parsed->data.empty() && parsed->data.front() == 20,
              "rapid writes preserve the newest value");
    }

    // Aging must survive the RE-QUEUE, not just the wait. onLinkTick re-asks
    // for NR/NB/notch every 1000 ms — the same period as kPriorityAgingMs — so
    // a collapse that rebuilds the entry restarts its clock and the group can
    // never age at all. The single-enqueue fixture above cannot see that.
    {
        IcomCivScheduler scheduler;
        const std::int64_t t0 = 1755300000000LL;   // a real epoch-ms, as the backend passes
        scheduler.enqueue(read("meter.s", 0x15, 0x02, Priority::ActiveMeter), t0);
        check(scheduler.takeNext(t0).has_value(), "aging fixture occupies the reply slot");
        scheduler.enqueue(read("control.nr", 0x16, 0x40, Priority::Control), t0);
        for (int tick = 1; tick <= 6; ++tick)
            scheduler.enqueue(read("control.nr", 0x16, 0x40, Priority::Control),
                              t0 + tick * 1000);
        check(scheduler.stats().queueDepth == 1,
              "a re-queued read collapses instead of accumulating");
        scheduler.enqueue(read("meter.power", 0x15, 0x11, Priority::ActiveMeter),
                          t0 + 6001);
        const auto aged = scheduler.takeNext(t0 + 6001);
        check(aged && aged->key == "control.nr",
              "a control re-queued every second still ages past fresh meter work");
    }

    // Aging tops out BELOW the PTT fallback poll. takeNext breaks an equal
    // priority tie on sequence and an aged entry always has the lower one, so
    // aging as far as Priority::Ptt would dispatch ahead of the keyed-state
    // poll rather than merely tying with it.
    {
        IcomCivScheduler scheduler;
        const std::int64_t t0 = 1755300000000LL;
        for (int i = 0; i < 10; ++i)
            scheduler.enqueue(read("c" + std::to_string(i), 0x16,
                                   static_cast<std::uint8_t>(0x40 + i),
                                   Priority::Control), t0);
        scheduler.enqueue(read("ptt", 0x1C, 0x00, Priority::Ptt), t0 + 5000);
        const auto next = scheduler.takeNext(t0 + 5000);
        check(next && next->key == "ptt",
              "a fresh PTT poll outranks background work aged for five seconds");
    }

    // A whole aged reconciliation burst must not drain ahead of TX meters.
    // Keep anti-starvation for controls, but interleave the two bands even
    // when PTT polls arrive between them.
    {
        IcomCivScheduler scheduler;
        for (int i = 0; i < 24; ++i) {
            scheduler.enqueue(read("background." + std::to_string(i), 0x16,
                                   static_cast<std::uint8_t>(i), Priority::Control), 0);
        }
        for (int i = 0; i < 6; ++i) {
            scheduler.enqueue(read("meter." + std::to_string(i), 0x15,
                                   static_cast<std::uint8_t>(i), Priority::ActiveMeter), 2000);
        }
        int meters = 0;
        int controls = 0;
        int consecutiveControls = 0;
        for (int i = 0; i < 15 && meters < 6; ++i) {
            const int now = 2000 + i * IcomCivScheduler::kSlotMs;
            if (i % 5 == 1) {
                scheduler.enqueue(read("ptt", 0x1C, 0, Priority::Ptt), now);
            }
            const auto next = scheduler.takeNext(now);
            check(next.has_value(), "meter burst fixture makes progress");
            if (!next) {
                break;
            }
            if (next->priority == Priority::Control) {
                ++controls;
                ++consecutiveControls;
                check(consecutiveControls <= 1,
                      "at most one aged background request runs between ready meters");
            } else if (next->priority == Priority::ActiveMeter) {
                ++meters;
                consecutiveControls = 0;
            } else {
                check(next->priority == Priority::Ptt, "PTT remains ahead of both bands");
            }
            check(scheduler.observe(ok(), now + 5) == IcomCivScheduler::Observation::Accepted,
                  "injected reply frees each burst slot");
        }
        check(meters == 6, "six TX meters drain within 375 ms despite 24 aged controls");
        check(controls > 0 && controls <= 6, "background reconciliation still progresses under meter load");
        const auto remainingControl = scheduler.takeNext(2400);
        check(remainingControl && remainingControl->priority == Priority::Control,
              "background work continues when no meter is ready");
        (void)scheduler.reset();
        scheduler.enqueue(read("background", 0x16, 0, Priority::Control), 0);
        scheduler.enqueue(read("meter", 0x15, 0, Priority::ActiveMeter), 2000);
        const auto afterReset = scheduler.takeNext(2000);
        check(afterReset && afterReset->priority == Priority::Control,
              "reset clears the interleave state for a new session");
    }

    // At a slower (50 ms) dispatch cadence, simple alternation still leaves
    // tail meters behind an excessive number of background transactions.
    {
        IcomCivScheduler scheduler;
        for (int i = 0; i < 20; ++i) {
            scheduler.enqueue(read("background." + std::to_string(i), 0x16,
                                   static_cast<std::uint8_t>(i), Priority::Control), 0);
        }
        for (int i = 0; i < 6; ++i) {
            scheduler.enqueue(read("meter." + std::to_string(i), 0x15,
                                   static_cast<std::uint8_t>(i), Priority::ActiveMeter), 2000);
        }
        int meters = 0;
        for (int i = 0; i < 7; ++i) {
            const int now = 2000 + i * 50;
            const auto next = scheduler.takeNext(now);
            check(next.has_value(), "slow cadence fixture dispatches");
            if (!next) {
                break;
            }
            if (now >= 2100) {
                check(next->priority == Priority::ActiveMeter,
                      "overdue meters suppress further background aging");
            }
            if (next->priority == Priority::ActiveMeter) {
                ++meters;
            }
            check(scheduler.observe(ok(), now + 15) == IcomCivScheduler::Observation::Accepted,
                  "injected reply frees each slow-cadence slot");
        }
        check(meters == 6, "slow cadence drains six meters by 300 ms");
    }

    // Yielding to overdue meters is a DELAY, NOT A HOLD.
    //
    // The two fixtures above enqueue a finite meter set, so the overdue
    // condition necessarily clears once those meters drain and background work
    // necessarily resumes. Production does not stop: MeterPoller re-arms each
    // meter from the ANSWER, so on a link slower than the meter demand there is
    // always a ready meter past its budget, the overdue condition never clears
    // on its own, and a floor conditioned on it alone starves control
    // reconciliation and the startup snapshot for the whole session rather than
    // deferring them. Hold the meters permanently overdue and assert that
    // background work still reaches the radio at the ceiling's rate.
    {
        IcomCivScheduler scheduler;
        for (int i = 0; i < 40; ++i) {
            scheduler.enqueue(read("background." + std::to_string(i), 0x16,
                                   static_cast<std::uint8_t>(i), Priority::Control), 0);
        }
        int meters = 0;
        int controls = 0;
        long lastControlMs = -1;
        long maxControlGapMs = 0;
        // A meter is always queued and always past kMeterQueueBudgetMs: each
        // one is enqueued a full budget in the past, exactly as a replenishing
        // poller presents them.
        for (int now = 2000; now <= 12000; now += 50) {
            scheduler.enqueue(read("meter." + std::to_string(now), 0x15,
                                   static_cast<std::uint8_t>(now % 8), Priority::ActiveMeter),
                              now - IcomCivScheduler::kMeterQueueBudgetMs);
            const auto next = scheduler.takeNext(now);
            if (!next) {
                continue;
            }
            if (next->priority == Priority::ActiveMeter) {
                ++meters;
            } else if (next->priority == Priority::Control) {
                ++controls;
                if (lastControlMs >= 0) {
                    maxControlGapMs = std::max(maxControlGapMs,
                                               static_cast<long>(now) - lastControlMs);
                }
                lastControlMs = now;
            }
            check(scheduler.observe(ok(), now + 5) == IcomCivScheduler::Observation::Accepted,
                  "injected reply frees each sustained-load slot");
        }
        check(meters > 0, "meters still dominate the sustained-load stream");
        check(controls > 0,
              "background work is not held off indefinitely by permanently overdue meters");
        // The ceiling is the contract: background work waits for it, and no
        // longer. Allow one dispatch slot of slack on either side.
        check(maxControlGapMs > 0
                  && maxControlGapMs <= IcomCivScheduler::kBackgroundStarvationCeilingMs
                         + IcomCivScheduler::kSlotMs * 2,
              "each background dispatch lands within the starvation ceiling");
        check(controls * 4 < meters,
              "the admitted background rate stays well below the meter rate");
    }

    // Two DIFFERENT registers share the coarse "mode" semantic key on purpose,
    // so a mode write supersedes an in-flight read of either. Coalescing must
    // not inherit that: sendConnectReadBurst queues 04 and 26 together because
    // 26 corrects 04 when they disagree.
    {
        IcomCivScheduler scheduler;
        const std::int64_t t0 = 1755300000000LL;
        scheduler.enqueue(read("mode", 0x04, 0x00, Priority::Maintenance), t0);
        scheduler.enqueue(read("mode", 0x26, 0x00, Priority::Maintenance), t0);
        check(scheduler.stats().queueDepth == 2,
              "reads of different registers are not coalesced by a shared key");
        scheduler.enqueue(read("meter.s", 0x15, 0x02, Priority::ActiveMeter), t0);
        scheduler.enqueue(read("meter.s", 0x15, 0x02, Priority::ActiveMeter), t0);
        check(scheduler.stats().queueDepth == 3,
              "while a true duplicate read still coalesces");
    }

    // A reply later than kReadTimeoutMs is still generation-checked. Without
    // this the identical frame is Stale at 349 ms and authoritative at 351 ms.
    {
        IcomCivScheduler scheduler;
        scheduler.enqueue(read("level.rf", 0x14, 0x02, Priority::Control), 0);
        check(scheduler.takeNext(0).has_value(), "late-reply fixture dispatches");
        scheduler.enqueue(write("level.rf", 0x14, 0x02, 99, Priority::Operator), 100);
        check(scheduler.observe(reply(0x14, 0x02, 0x50), 360)
                  == IcomCivScheduler::Observation::Stale,
              "a reply that misses the timeout is still rejected against a "
              "newer write generation");
        check(scheduler.recentTransactions().size() == 2
                  && scheduler.recentTransactions().back().completion
                      == IcomCivScheduler::Completion::LateStaleReply
                  && scheduler.stats().lateReplies == 1,
              "a superseded late reply is distinguished from its timeout");
    }
    {
        // ...but a merely slow reply with nothing newer behind it is not
        // reportable as stale; the backend should still adopt it.
        IcomCivScheduler scheduler;
        scheduler.enqueue(read("level.rf", 0x14, 0x02, Priority::Control), 0);
        check(scheduler.takeNext(0).has_value(), "slow-reply fixture dispatches");
        check(scheduler.observe(reply(0x14, 0x02, 0x50), 360)
                  == IcomCivScheduler::Observation::Unmatched,
              "a late reply with no newer intent behind it is not marked stale");
        check(scheduler.recentTransactions().size() == 2
                  && scheduler.recentTransactions().back().completion
                      == IcomCivScheduler::Completion::LateReply,
              "an authoritative slow reply remains visible as late, not stale");
    }

    {
        IcomCivScheduler scheduler;
        IcomCivScheduler::Request noReply;
        noReply.frame = buildFrameSub(0xB6, 0x1C, 0x00, std::array<std::uint8_t, 1>{0});
        noReply.key = "ptt";
        noReply.priority = Priority::Emergency;
        noReply.expectsReply = false;
        scheduler.enqueue(std::move(noReply), 8000);
        check(scheduler.takeNext(8000).has_value(),
              "response-free fail-safe command dispatches");
        check(scheduler.recentTransactions().size() == 1
                  && scheduler.recentTransactions().back().completion
                      == IcomCivScheduler::Completion::NoReply
                  && scheduler.recentTransactions().back().responseMs == -1,
              "response-free commands have an explicit terminal outcome");
    }

    {
        IcomCivScheduler scheduler;
        scheduler.enqueue(read("queued", 0x16, 0x40, Priority::Control), 0);
        scheduler.enqueue(read("in-flight", 0x15, 0x02, Priority::ActiveMeter), 0);
        check(scheduler.takeNext(0).has_value(), "reset fixture dispatches one request");
        const IcomCivScheduler::ResetResult reset = scheduler.reset();
        check(reset.requests.size() == 2,
              "reset explicitly reports every queued and in-flight cancellation");
        check(std::count_if(reset.requests.begin(), reset.requests.end(),
                            [](const IcomCivScheduler::TerminalRequest& request) {
                                return request.outcome
                                        == IcomCivScheduler::TerminalOutcome::Cancelled
                                    && request.wasInFlight;
                            }) == 1,
              "reset identifies the cancelled in-flight request");
        check(std::count_if(reset.requests.begin(), reset.requests.end(),
                            [](const IcomCivScheduler::TerminalRequest& request) {
                                return request.outcome
                                        == IcomCivScheduler::TerminalOutcome::Cancelled
                                    && !request.wasInFlight;
                            }) == 1,
              "reset identifies the cancelled queued request");
        check(scheduler.idle(), "reset leaves the scheduler idle");
    }

    {
        IcomCivScheduler scheduler;
        scheduler.enqueue(read("failed-in-flight", 0x15, 0x02,
                               Priority::ActiveMeter), 0);
        scheduler.enqueue(read("failed-queued", 0x16, 0x40,
                               Priority::Control), 0);
        check(scheduler.takeNext(0).has_value(), "failure fixture dispatches");
        const IcomCivScheduler::ResetResult reset =
            scheduler.reset(IcomCivScheduler::TerminalOutcome::Failed);
        check(reset.requests.size() == 2
                  && std::ranges::all_of(
                      reset.requests,
                      [](const IcomCivScheduler::TerminalRequest& request) {
                          return request.outcome
                              == IcomCivScheduler::TerminalOutcome::Failed;
                      }),
              "a command-plane failure terminates every queued and in-flight "
              "request distinctly from operator cancellation");
        check(std::ranges::any_of(
                  reset.requests,
                  [](const IcomCivScheduler::TerminalRequest& request) {
                      return request.key == "failed-in-flight" && request.wasInFlight;
                  })
                  && std::ranges::any_of(
                      reset.requests,
                      [](const IcomCivScheduler::TerminalRequest& request) {
                          return request.key == "failed-queued" && !request.wasInFlight;
                      }),
              "failure accounting preserves each request identity and lifecycle state");
    }

    if (g_failures == 0) {
        std::printf("icom_civ_scheduler_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "icom_civ_scheduler_test: %d failure(s)\n", g_failures);
    return 1;
}
