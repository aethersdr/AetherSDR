// HL2 forward power: the window maximum through MetisClient's receive loop.
// Socket-free: synthetic EP6 datagrams go into handleDatagram through
// MetisClientTestAccess. hl2_metis_protocol_test owns the ForwardPowerWindow
// rule; this owns its wiring (observe() in the loop, stamp-and-clear at emit).
// The 100 ms emit clock is restarted (holdEmit) or invalidated (dueEmit) by hand.

#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QObject>

#include <cstdint>
#include <cstdio>
#include <span>
#include <utility>
#include <vector>

namespace AetherSDR::hl2 {
struct MetisClientTestAccess {
    static void setStreaming(MetisClient& client) { client.m_running = true; }
    static void feedDatagram(MetisClient& client, std::span<const std::uint8_t> bytes)
    {
        client.handleDatagram(bytes);
    }
    static void holdEmit(MetisClient& client) { client.m_telemetryEmitClock.restart(); }
    static void dueEmit(MetisClient& client) { client.m_telemetryEmitClock.invalidate(); }
};
}  // namespace AetherSDR::hl2

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// One C&C slot: the C0 byte and the 32-bit DATA word in C1..C4.
struct Slot {
    std::uint8_t c0;
    std::uint32_t data;
};

// Free-running telemetry: C0[6:3] is the response address, ACK clear.
static constexpr Slot raddr(std::uint8_t addr, std::uint32_t data)
{
    return Slot{static_cast<std::uint8_t>(addr << 3), data};
}
// An ACK: C0[7] set, C0[6:1] the COMMAND address being answered.
static constexpr Slot ack(std::uint8_t cmd, std::uint32_t data)
{
    return Slot{static_cast<std::uint8_t>(0x80 | (cmd << 1)), data};
}
// Forward power is DATA[15:0] of RADDR 1; DATA[31:16] is temperature.
static constexpr Slot fwd(std::uint16_t counts)
{
    return raddr(0x01, (1234u << 16) | counts);
}

// One EP6 datagram with a C&C slot in each of its two frames. The IQ payload
// is zeros: the telemetry path is the subject.
static std::vector<std::uint8_t> makeEp6(std::uint32_t seq, Slot a, Slot b)
{
    std::vector<std::uint8_t> pkt(kUsbPacketSize, 0);
    pkt[0] = 0xEF; pkt[1] = 0xFE; pkt[2] = 0x01; pkt[3] = 0x06;
    pkt[4] = static_cast<std::uint8_t>((seq >> 24) & 0xFF);
    pkt[5] = static_cast<std::uint8_t>((seq >> 16) & 0xFF);
    pkt[6] = static_cast<std::uint8_t>((seq >> 8) & 0xFF);
    pkt[7] = static_cast<std::uint8_t>(seq & 0xFF);
    const std::pair<std::size_t, Slot> frames[2] = {{8, a}, {8 + kFrameSize, b}};
    for (const auto& [fs, s] : frames) {
        pkt[fs] = pkt[fs + 1] = pkt[fs + 2] = 0x7F;           // SYNC
        pkt[fs + 3] = s.c0;
        pkt[fs + 4] = static_cast<std::uint8_t>((s.data >> 24) & 0xFF);
        pkt[fs + 5] = static_cast<std::uint8_t>((s.data >> 16) & 0xFF);
        pkt[fs + 6] = static_cast<std::uint8_t>((s.data >> 8) & 0xFF);
        pkt[fs + 7] = static_cast<std::uint8_t>(s.data & 0xFF);
    }
    return pkt;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    MetisClient c;
    MetisClientTestAccess::setStreaming(c);
    std::vector<Hl2Telemetry> emitted;
    // No context object, so a direct connection: the emit is on this thread
    // and lands in the vector before feedDatagram returns.
    QObject::connect(&c, &MetisClient::telemetryUpdated,
                     [&emitted](const Hl2Telemetry& t) { emitted.push_back(t); });

    std::uint32_t seq = 0;
    auto hold = [&](Slot a, Slot b) {
        MetisClientTestAccess::holdEmit(c);
        MetisClientTestAccess::feedDatagram(c, makeEp6(seq++, a, b));
    };
    auto due = [&](Slot a, Slot b) {
        MetisClientTestAccess::dueEmit(c);
        MetisClientTestAccess::feedDatagram(c, makeEp6(seq++, a, b));
    };

    // ---- 1 · the peak lands mid-window, the window ends on a trough ----
    //
    // Interleaved with the two things that must NOT count: a RADDR 2 word and
    // an ACK for command 0x01, both carrying a value louder than the real
    // peak. Either one leaking into the window would win the maximum.
    {
        hold(fwd(300), raddr(0x02, (4000u << 16) | 4000u));
        hold(fwd(3200), ack(0x01, 0x0FFFu));
        hold(fwd(900), raddr(0x02, (4000u << 16) | 4000u));
        check(emitted.empty(), "held datagrams accumulate without emitting");
        due(raddr(0x02, 0), fwd(450));
        check(emitted.size() == 1, "the due datagram emits exactly once");
        if (emitted.size() == 1) {
            const Hl2Telemetry& t = emitted.back();
            check(t.forwardPowerPeakRaw.value_or(-1) == 3200,
                  "the emit carries the window PEAK (3200), not the last value (450)");
            check(t.forwardPowerRaw.value_or(-1) == 450,
                  "forwardPowerRaw keeps its last-value meaning beside the peak");
            check(t.forwardPowerSamples == 4,
                  "four non-ACK RADDR 1 responses: RADDR 2 and the ACK are not counted");
        }
    }

    // ---- 2 · a window with no RADDR 1 publishes nullopt, not a stale peak ----
    {
        due(raddr(0x02, 0), raddr(0x03, 0));
        check(emitted.size() == 2, "a telemetry-only window still emits");
        if (emitted.size() == 2) {
            const Hl2Telemetry& t = emitted.back();
            check(!t.forwardPowerPeakRaw.has_value(),
                  "no RADDR 1 in the window -> nullopt, never last window's 3200");
            check(t.forwardPowerSamples == 0, "and a zero count to say so");
            // The last value is sticky by design (Hl2Telemetry::apply merges);
            // that is what Hl2Backend falls back to when the peak is nullopt.
            check(t.forwardPowerRaw.value_or(-1) == 450,
                  "forwardPowerRaw still holds the last value seen");
        }
    }

    // ---- 3 · the window cleared: a quiet window after a loud one ----
    {
        hold(fwd(120), raddr(0x02, 0));
        due(fwd(100), raddr(0x02, 0));
        check(emitted.size() == 3, "the next window emits once");
        if (emitted.size() == 3) {
            const Hl2Telemetry& t = emitted.back();
            check(t.forwardPowerPeakRaw.value_or(-1) == 120,
                  "the quiet window reports its own peak, not the earlier 3200");
            check(t.forwardPowerSamples == 2, "and counts only its own responses");
        }
    }

    // ---- 4 · ACKs alone never reach the window ----
    //
    // A datagram of two ACKs for command 0x01 carries no free-running
    // telemetry, so it neither emits nor feeds the window; the next emit
    // must show the window empty.
    {
        due(ack(0x01, 0x0FFFu), ack(0x01, 0x0FFFu));
        check(emitted.size() == 3, "an ACK-only datagram is not telemetry and does not emit");
        due(raddr(0x02, 0), raddr(0x03, 0));
        check(emitted.size() == 4, "the next telemetry datagram emits");
        if (emitted.size() == 4) {
            const Hl2Telemetry& t = emitted.back();
            check(!t.forwardPowerPeakRaw.has_value(),
                  "an ACK's echo on command address 1 never became a forward-power peak");
            check(t.forwardPowerSamples == 0, "and was never counted");
        }
    }

    // ---- 5 · the skipped count rides the same emit and clears with the window ----
    //
    // A RADDR-1 word with a temperature half of 0 is not a measurement.
    {
        hold(raddr(0x01, 2944u), fwd(300));
        due(raddr(0x01, 0u), fwd(450));
        check(emitted.size() == 5, "the window with marked responses emits once");
        if (emitted.size() == 5) {
            const Hl2Telemetry& t = emitted.back();
            check(t.forwardPowerSkipped == 2 && t.forwardPowerSamples == 2,
                  "two marked responses are reported as skipped, beside the two kept");
            check(t.forwardPowerPeakRaw.value_or(-1) == 450,
                  "and the peak is over the kept ones (450), not the marked 2944");
        }
        due(fwd(100), raddr(0x02, 0));
        check(emitted.size() == 6 && emitted.back().forwardPowerSkipped == 0
                  && emitted.back().forwardPowerSamples == 1,
              "the next window starts its skipped count at zero");
        hold(raddr(0x01, 2944u), raddr(0x01, 5u));
        due(raddr(0x01, 0u), raddr(0x02, 0));
        check(emitted.size() == 7 && emitted.back().forwardPowerSkipped == 3
                  && emitted.back().forwardPowerSamples == 0
                  && !emitted.back().forwardPowerPeakRaw.has_value(),
              "a window of marked responses only: a skipped count, no kept sample, no peak");
    }

    if (g_failures == 0)
        std::printf("hl2_fwd_window_wiring_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
