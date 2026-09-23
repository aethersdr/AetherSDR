// Contour ShuttleXpress / ShuttlePro v2 report decoding (#5927), including
// several edges in one report.
//
// The ShuttleXpress byte sequences are real captures from a ShuttleXpress
// (VID 0B33 / PID 0020) read with hid_read() on Windows 11. The ShuttlePro v2
// layout is inferred from them (the Xpress uses the Pro's button 5-9 bits).

#include "core/HidDeviceParser.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

using AetherSDR::HidEvent;
using AetherSDR::ShuttleProV2Parser;
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

} // namespace

int main()
{
    testXpressAllButtons();
    testXpressShortReport();
    testXpressJogAndRing();
    testProButtons();
    testSimultaneousEdges();

    if (g_failed) {
        std::printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
