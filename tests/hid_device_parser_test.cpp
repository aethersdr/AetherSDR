// Contour ShuttleXpress / ShuttlePro v2 report decoding (#5927), including
// several edges in one report, and the shuttle ring (#5928): its position
// decoding and the rate integrator that turns a held position into steps.
//
// The ShuttleXpress byte sequences are real captures from a ShuttleXpress
// (VID 0B33 / PID 0020) read with hid_read() on Windows 11. The ShuttlePro v2
// layout is inferred from them (the Xpress uses the Pro's button 5-9 bits).

#include "core/HidDeviceParser.h"
#include "core/ShuttleRateIntegrator.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using AetherSDR::HidEvent;
using AetherSDR::ShuttleProV2Parser;
using AetherSDR::ShuttleRateIntegrator;
using AetherSDR::ShuttleXpressParser;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok)
        ++g_failed;
}

using Report = std::array<uint8_t, 5>;

HidEvent feed(AetherSDR::HidDeviceParser& p, const Report& r)
{
    return p.parse(r.data(), r.size());
}

bool isButton(const HidEvent& e, int button, int action)
{
    return e.type == HidEvent::Button && e.button == button && e.action == action;
}

// Every event one report produces, the way HidEncoderManager::poll() drains
// them: parse() first, then nextPending() until None.
std::vector<HidEvent> drain(AetherSDR::HidDeviceParser& p, const Report& r)
{
    std::vector<HidEvent> out;
    for (auto e = p.parse(r.data(), r.size()); e.type != HidEvent::None; e = p.nextPending())
        out.push_back(e);
    return out;
}

// Every ShuttleXpress button must press and release as its own number.
void testXpressAllButtons()
{
    // Captured press reports for physical buttons 1..5, left to right.
    static const Report kPress[5] = {
        {0x00, 0x25, 0x00, 0x10, 0x00},
        {0x00, 0x25, 0x00, 0x20, 0x00},
        {0x00, 0x25, 0x00, 0x40, 0x00},
        {0x00, 0x25, 0x00, 0x80, 0x00},
        {0x00, 0x25, 0x00, 0x00, 0x01},
    };
    static const Report kIdle = {0x00, 0x25, 0x00, 0x00, 0x00};

    ShuttleXpressParser p;
    feed(p, kIdle);  // first report primes the jog baseline

    bool ok = true;
    for (int i = 0; i < 5; ++i) {
        ok = ok && isButton(feed(p, kPress[i]), i + 1, 0);
        ok = ok && isButton(feed(p, kIdle), i + 1, 1);
    }
    report("ShuttleXpress: buttons 1-5 press/release as 1-5", ok);
}

// Byte 4 must be required: a 4-byte read cannot carry button 5.
void testXpressShortReport()
{
    ShuttleXpressParser p;
    const uint8_t shortBuf[4] = {0x00, 0x25, 0x00, 0x10};
    report("ShuttleXpress: short report ignored",
           p.parse(shortBuf, sizeof(shortBuf)).type == HidEvent::None);
}

// Ring-only reports (byte 0 changes, jog unchanged) must not produce events,
// and the jog wheel must keep working next to the button fix.
void testXpressJogAndRing()
{
    ShuttleXpressParser p;
    feed(p, {0x00, 0xc8, 0x00, 0x00, 0x00});

    const bool ringQuiet = feed(p, {0x07, 0xc8, 0x00, 0x00, 0x00}).type == HidEvent::None
                        && feed(p, {0xf9, 0xc8, 0x00, 0x00, 0x00}).type == HidEvent::None;
    report("ShuttleXpress: ring-only report produces no event", ringQuiet);

    const HidEvent up = feed(p, {0x00, 0xc9, 0x00, 0x00, 0x00});
    const HidEvent wrap = [&] {
        feed(p, {0x00, 0xff, 0x00, 0x00, 0x00});
        return feed(p, {0x00, 0x01, 0x00, 0x00, 0x00});
    }();
    report("ShuttleXpress: jog +1", up.type == HidEvent::Rotate && up.steps == 1);
    report("ShuttleXpress: jog wraps 0xff -> 0x01 as +2",
           wrap.type == HidEvent::Rotate && wrap.steps == 2);
}

// ShuttlePro v2: byte 3 = buttons 1-8, byte 4 = buttons 9-15.
void testProButtons()
{
    static const Report kIdle = {0x00, 0x10, 0x00, 0x00, 0x00};
    ShuttleProV2Parser p;
    feed(p, kIdle);

    bool ok = true;
    for (int b = 0; b < 15; ++b) {
        Report press = kIdle;
        if (b < 8)
            press[3] = static_cast<uint8_t>(1 << b);
        else
            press[4] = static_cast<uint8_t>(1 << (b - 8));
        ok = ok && isButton(feed(p, press), b + 1, 0);
        ok = ok && isButton(feed(p, kIdle), b + 1, 1);
    }
    report("ShuttlePro v2: buttons 1-15 press/release as 1-15", ok);
}

// Several edges in one report must all be delivered, in button order, and a
// jog change in the same report must follow them rather than wait for the
// next report.
void testSimultaneousEdges()
{
    static const Report kIdle = {0x00, 0x25, 0x00, 0x00, 0x00};

    {
        ShuttleXpressParser p;
        drain(p, kIdle);
        const auto press = drain(p, {0x00, 0x25, 0x00, 0x30, 0x00});    // buttons 1+2
        const auto release = drain(p, kIdle);
        report("ShuttleXpress: buttons 1+2 pressed together -> two presses",
               press.size() == 2 && isButton(press[0], 1, 0) && isButton(press[1], 2, 0));
        report("ShuttleXpress: buttons 1+2 released together -> two releases",
               release.size() == 2 && isButton(release[0], 1, 1) && isButton(release[1], 2, 1));
    }
    {
        // Button 4 (byte 3) and button 5 (byte 4) straddle the byte boundary.
        ShuttleXpressParser p;
        drain(p, kIdle);
        const auto press = drain(p, {0x00, 0x25, 0x00, 0x80, 0x01});
        const auto release = drain(p, kIdle);
        report("ShuttleXpress: buttons 4+5 across bytes 3/4 both press and release",
               press.size() == 2 && isButton(press[0], 4, 0) && isButton(press[1], 5, 0)
               && release.size() == 2 && isButton(release[0], 4, 1) && isButton(release[1], 5, 1));
    }
    {
        // A press and a release in the same report (button 1 up, button 3 down).
        ShuttleXpressParser p;
        drain(p, kIdle);
        drain(p, {0x00, 0x25, 0x00, 0x10, 0x00});
        const auto swap = drain(p, {0x00, 0x25, 0x00, 0x40, 0x00});
        report("ShuttleXpress: release 1 + press 3 in one report",
               swap.size() == 2 && isButton(swap[0], 1, 1) && isButton(swap[1], 3, 0));
    }
    {
        ShuttleXpressParser p;
        drain(p, kIdle);
        const auto both = drain(p, {0x00, 0x27, 0x00, 0x20, 0x00});     // button 2 + jog +2
        report("ShuttleXpress: button and jog in one report -> button then jog",
               both.size() == 2 && isButton(both[0], 2, 0)
               && both[1].type == HidEvent::Rotate && both[1].steps == 2);
    }
    {
        // ShuttlePro v2 buttons 8 (byte 3 bit 7) and 9 (byte 4 bit 0).
        ShuttleProV2Parser p;
        drain(p, kIdle);
        const auto press = drain(p, {0x00, 0x25, 0x00, 0x80, 0x01});
        const auto release = drain(p, kIdle);
        report("ShuttlePro v2: buttons 8+9 across bytes 3/4 both press and release",
               press.size() == 2 && isButton(press[0], 8, 0) && isButton(press[1], 9, 0)
               && release.size() == 2 && isButton(release[0], 8, 1) && isButton(release[1], 9, 1));
    }
    {
        // A short report must not replay events queued by the previous one.
        ShuttleXpressParser p;
        drain(p, kIdle);
        p.parse(Report{0x00, 0x25, 0x00, 0x30, 0x00}.data(), 5);     // leave button 2 queued
        const uint8_t shortBuf[4] = {0x00, 0x25, 0x00, 0x00};
        const bool quiet = p.parse(shortBuf, sizeof(shortBuf)).type == HidEvent::None
                        && p.nextPending().type == HidEvent::None;
        report("ShuttleXpress: short report clears the queue", quiet);
    }
}

// Ring position from byte 0, from the captured sweep 0 -> +7 -> 0 -> -7.
void testShuttlePosition()
{
    ShuttleXpressParser p;
    report("Shuttle: no ring before the first report", p.hasShuttle() && p.shuttlePosition() == 0);

    bool ok = true;
    for (int pos = -7; pos <= 7; ++pos) {
        p.parse(Report{static_cast<uint8_t>(static_cast<int8_t>(pos)), 0xc8, 0x00, 0x00, 0x00}.data(), 5);
        ok = ok && p.shuttlePosition() == pos;
    }
    report("Shuttle: 0xf9..0x07 decode as -7..+7", ok);

    p.parse(Report{0x7f, 0xc8, 0x00, 0x00, 0x00}.data(), 5);
    const bool clampHigh = p.shuttlePosition() == 7;
    p.parse(Report{0x80, 0xc8, 0x00, 0x00, 0x00}.data(), 5);
    report("Shuttle: out-of-range byte clamps to +/-7", clampHigh && p.shuttlePosition() == -7);

    // The ring keeps its position when a button changes in the same report.
    p.parse(Report{0x03, 0xc8, 0x00, 0x20, 0x00}.data(), 5);
    report("Shuttle: position survives a simultaneous button edge", p.shuttlePosition() == 3);

    ShuttleProV2Parser pro;
    pro.parse(Report{0xfd, 0x10, 0x00, 0x00, 0x00}.data(), 5);
    report("Shuttle: ShuttlePro v2 decodes the ring too", pro.hasShuttle() && pro.shuttlePosition() == -3);
}

// Integrate one second of 40 ms ticks at a fixed position.
int stepsInOneSecond(ShuttleRateIntegrator& r, int stepHz, double speed = 1.0,
                     double maxRate = 1e9)
{
    int total = 0;
    for (int i = 0; i < 25; ++i)
        total += r.tick(0.040, stepHz, speed, maxRate);
    return total;
}

void testRateIntegrator()
{
    // Every new deflection held for 60 ms applies one step (the jog-like
    // response), so the per-second counts below include that one step.
    {
        ShuttleRateIntegrator r;
        r.setPosition(1);
        const int t1 = r.tick(0.040, 10, 1.0, 1e9);
        const int t2 = r.tick(0.040, 10, 1.0, 1e9);
        report("Rate: leaving centre steps once held 60 ms (second 40 ms tick)",
               t1 == 0 && t2 == 1);
    }
    {
        // Measured release: the spring overshoots through -1/-2 for 30-35 ms.
        ShuttleRateIntegrator r;
        r.setPosition(-1);
        const int overshoot = r.tick(0.035, 10, 1.0, 1e9);
        r.setPosition(0);
        report("Rate: 35 ms snap-back overshoot never steps backwards",
               overshoot == 0 && r.tick(0.040, 10, 1.0, 1e9) == 0);
    }
    {
        // The rate is in Hz/s: full deflection moves ~100 kHz per second at
        // any step size (the review point on #5928).
        ShuttleRateIntegrator r10, r1k;
        r10.setPosition(7);
        r1k.setPosition(7);
        const int s10 = stepsInOneSecond(r10, 10) - 1;
        const int s1k = stepsInOneSecond(r1k, 1000) - 1;
        report("Rate: +7 is ~100 kHz/s at a 10 Hz step",
               std::abs(s10 * 10 - 100'000) <= 10);
        report("Rate: +7 is ~100 kHz/s at a 1 kHz step",
               std::abs(s1k * 1000 - 100'000) <= 1000);
    }
    {
        // Slow creep: 20 Hz/s at a 10 Hz step is 2 steps per second, carried
        // across ticks rather than lost as a fraction every 40 ms.
        ShuttleRateIntegrator r;
        r.setPosition(1);
        report("Rate: +1 creeps 2 steps/s at a 10 Hz step (remainder carried)",
               stepsInOneSecond(r, 10) == 1 + 2);
    }
    {
        // With a large step the (|pos| + 1) steps/s floor keeps the first
        // detents moving and distinct instead of taking tens of seconds.
        ShuttleRateIntegrator r1, r2;
        r1.setPosition(1);
        r2.setPosition(2);
        report("Rate: 1 kHz step, +1 still moves 2 steps/s",
               stepsInOneSecond(r1, 1000) == 1 + 2);
        report("Rate: 1 kHz step, +2 moves faster than +1 (3 steps/s)",
               stepsInOneSecond(r2, 1000) == 1 + 3);
    }
    {
        ShuttleRateIntegrator r;
        r.setPosition(-4);
        report("Rate: negative position tunes down",
               stepsInOneSecond(r, 100) == -(1 + 20));   // 2 kHz/s at 100 Hz
    }
    {
        ShuttleRateIntegrator r;
        r.setPosition(7);
        report("Rate: Slow/Fast multiplier and the max-rate cap",
               stepsInOneSecond(r, 10, 2.0, 1000.0) == 1 + 100);   // capped at 1 kHz/s
    }
    {
        // Moving further out on the same side is not a new deflection.
        ShuttleRateIntegrator r;
        r.setPosition(2);
        r.tick(0.080, 10, 1.0, 1e9);     // first step + 8 Hz carried
        r.setPosition(3);
        report("Rate: 2 -> 3 on the same side does not add a first step",
               r.tick(0.040, 10, 1.0, 1e9) == 2);   // 8 + 20 Hz = 2 steps
    }
    {
        // A remainder built up in one direction must not leak into the other.
        ShuttleRateIntegrator r;
        r.setPosition(1);
        r.tick(0.080, 10, 1.0, 1e9);     // first step, 1.6 Hz carried
        r.setPosition(-1);
        int total = 0;
        for (int i = 0; i < 12; ++i)     // first step, then 12 * 0.8 Hz < one step
            total += r.tick(0.040, 10, 1.0, 1e9);
        report("Rate: reversal drops the remainder", total == -1);
    }
    {
        ShuttleRateIntegrator r;
        r.setPosition(3);
        r.setPosition(0);
        report("Rate: centred ring produces nothing", r.tick(0.040, 10, 1.0, 1e9) == 0);
        r.setPosition(5);
        report("Rate: bad step size produces nothing", r.tick(0.040, 0, 1.0, 1e9) == 0);
    }
}

} // namespace

int main()
{
    testXpressAllButtons();
    testXpressShortReport();
    testXpressJogAndRing();
    testProButtons();
    testSimultaneousEdges();
    testShuttlePosition();
    testRateIntegrator();

    if (g_failed) {
        std::printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
