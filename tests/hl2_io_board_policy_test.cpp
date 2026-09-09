// Socket-free scheduling regressions; the backend uses this state on both
// the tune path and the QTimer timeout path.
#include "core/backends/hl2/Hl2IoBoardPolicy.h"
#include <cstdio>

using namespace AetherSDR::hl2;
static int failures = 0;
static void check(bool ok, const char* message)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

int main()
{
    IoBoardSchedule schedule;
    check(schedule.request(true, false, true, 7'100'000) == IoBoardAction::Send,
          "connect sends immediately");
    check(schedule.request(true, true, false, 7'101'000) == IoBoardAction::Coalesce,
          "same-band sweep coalesces");
    check(schedule.request(true, true, true, 14'225'000) == IoBoardAction::Send,
          "40m to 20m beats the cooldown, including during MOX/TUNE");
    check(schedule.takePending() == 0,
          "timeout after band change must not restore the pending 40m value");

    (void)schedule.request(true, true, false, 14'226'000);
    (void)schedule.request(true, true, false, 14'227'000);
    check(schedule.takePending() == 14'227'000, "timeout sends latest same-band value");
    check(schedule.takePending() == 0, "timeout consumes pending work once");

    (void)schedule.request(true, true, false, 14'228'000);
    check(schedule.request(false, false, true, 7'100'000) == IoBoardAction::DropDisconnected,
          "disconnected leading edge refuses to send");
    check(schedule.takePending() == 0, "disconnected request discards pending work");
    (void)schedule.request(true, true, false, 14'229'000);
    schedule.reset();
    check(schedule.takePending() == 0, "link loss cancels pending work");
    check(schedule.request(true, false, true, 7'100'000) == IoBoardAction::Send,
          "reconnect immediately pushes the current frequency");

    for (int bits = 0; bits < 8; ++bits) {
        const bool connected = bits & 1;
        const bool throttled = bits & 2;
        const bool bandChanged = bits & 4;
        const IoBoardAction result = ioBoardAction(connected, throttled, bandChanged);
        if (!connected) {
            check(result == IoBoardAction::DropDisconnected, "all disconnected states drop");
        } else if (bandChanged || !throttled) {
            check(result == IoBoardAction::Send, "connected band changes and idle sends proceed");
        } else {
            check(result == IoBoardAction::Coalesce, "only same-band cooldown coalesces");
        }
    }
    return failures == 0 ? 0 : 1;
}
