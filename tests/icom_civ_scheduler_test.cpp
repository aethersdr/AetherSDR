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

    if (g_failures == 0) {
        std::printf("icom_civ_scheduler_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "icom_civ_scheduler_test: %d failure(s)\n", g_failures);
    return 1;
}
