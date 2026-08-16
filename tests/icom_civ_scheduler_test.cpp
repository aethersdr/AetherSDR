#include "core/backends/icom/IcomCivScheduler.h"

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
        scheduler.enqueue(read("meter.s", 0x15, 0x02, Priority::ActiveMeter), 1000);
        scheduler.enqueue(read("meter.s", 0x15, 0x02, Priority::ActiveMeter), 1000);
        check(scheduler.stats().queueDepth == 1, "duplicate reads coalesce");
        const auto first = scheduler.takeNext(1000);
        check(first && first->key == "meter.s", "first read dispatches");
        check(!scheduler.takeNext(1100), "only one reply-bearing read is outstanding");
        check(scheduler.observe(reply(0x15, 0x02, 0), 1110)
                  == IcomCivScheduler::Observation::Accepted,
              "matching reply completes the outstanding read");
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
    }

    if (g_failures == 0) {
        std::printf("icom_civ_scheduler_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "icom_civ_scheduler_test: %d failure(s)\n", g_failures);
    return 1;
}
