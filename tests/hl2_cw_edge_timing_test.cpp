// #5065: a CW edge that carries `scheduledAt` reaches the EP2 stream at its
// own sample (plus MetisClient's fixed lead), not when the queued call lands
// on the I/O thread. Socket-free: frames come from buildNextControlPacket(),
// edges are delivered "late" by holding them until the frame cursor has passed
// their arrival instant, which is what a GUI or I/O stall does to them.
// Every length is measured from the I channel of the frames themselves.
//
// v2: a key-down carries its own key-up instant (`releaseAt`) and is never
// dropped for lateness; a late one shifts the rest of the over so every element
// keeps its length, and a shift past kCwMaxEdgeShiftMs refuses the over.

#include "TestSettingsProfile.h"
#include "TxTestAuthority.h"
#include "core/backends/CwControlRequest.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace AetherSDR::hl2 {
struct MetisClientTestAccess {
    static void setStreaming(MetisClient& client) { client.m_running = true; }
    // EP2 sample 0 at `t`, the next frame built being sample 0: what start() does.
    static void anchor(MetisClient& client, std::chrono::steady_clock::time_point t)
    {
        client.m_cwAnchor = t;
        client.m_cwFrameSample = 0;
    }
    static constexpr int rateHz() { return MetisClient::kEp2AudioRateHz; }
    static constexpr std::int64_t leadSamples() { return MetisClient::kCwEdgeLeadSamples; }
    static constexpr std::int64_t capSamples() { return MetisClient::kCwMaxScheduledElementSamples; }
    static constexpr std::size_t capacity() { return MetisClient::kCwScheduleCapacity; }
    static constexpr std::int64_t maxShiftSamples() { return MetisClient::kCwMaxEdgeShiftSamples; }
    static std::size_t pending(const MetisClient& client) { return client.m_cwScheduleCount; }
    static std::int64_t frameSample(const MetisClient& client) { return client.m_cwFrameSample; }
};

struct Hl2TxGateTestAccess {
    static void prepare(Hl2Backend& backend, std::chrono::steady_clock::time_point anchor)
    {
        backend.m_txAllowed = true;
        backend.m_txTailHoldMs = 60 * 60 * 1000;   // parked; nothing here waits for it
        QMetaObject::invokeMethod(backend.m_metis, [metis = backend.m_metis, anchor] {
            metis->enableTransmit(true);
            MetisClientTestAccess::setStreaming(*metis);
            MetisClientTestAccess::anchor(*metis, anchor);
        }, Qt::BlockingQueuedConnection);
        backend.setSliceMode(0, QStringLiteral("CW"));
    }
    static std::array<std::uint8_t, kUsbPacketSize> packet(Hl2Backend& backend)
    {
        std::array<std::uint8_t, kUsbPacketSize> packet{};
        QMetaObject::invokeMethod(backend.m_metis, [&packet, metis = backend.m_metis] {
            packet = metis->buildNextControlPacket();
        }, Qt::BlockingQueuedConnection);
        return packet;
    }
    static std::size_t pending(Hl2Backend& backend)
    {
        std::size_t n = 0;
        QMetaObject::invokeMethod(backend.m_metis, [&n, metis = backend.m_metis] {
            n = MetisClientTestAccess::pending(*metis);
        }, Qt::BlockingQueuedConnection);
        return n;
    }
    static bool hangActive(const Hl2Backend& backend) { return backend.m_cwHangTimer->isActive(); }
    static int hangIntervalMs(const Hl2Backend& backend) { return backend.m_cwHangTimer->interval(); }
    static int hangDelayMs(const Hl2Backend& backend) { return backend.m_cwHangDelayMs; }
    static bool cwAutoKeyed(const Hl2Backend& backend) { return backend.m_cwAutoKeyed; }
    static std::int64_t shiftSamples(Hl2Backend& backend)
    {
        std::int64_t n = 0;
        QMetaObject::invokeMethod(backend.m_metis, [&n, metis = backend.m_metis] {
            n = metis->cwShiftSamples();
        }, Qt::BlockingQueuedConnection);
        return n;
    }
    static std::int64_t frameMs(Hl2Backend& backend)
    {
        std::int64_t n = 0;
        QMetaObject::invokeMethod(backend.m_metis, [&n, metis = backend.m_metis] {
            n = MetisClientTestAccess::frameSample(*metis) * 1000 / MetisClientTestAccess::rateHz();
        }, Qt::BlockingQueuedConnection);
        return n;
    }
    static std::uint64_t refusedDowns(Hl2Backend& backend)
    {
        std::uint64_t n = 0;
        QMetaObject::invokeMethod(backend.m_metis, [&n, metis = backend.m_metis] {
            n = metis->cwRefusedDowns();
        }, Qt::BlockingQueuedConnection);
        return n;
    }
    // What the break-in hang's unkey does on the I/O thread: the over ends.
    static void endOver(Hl2Backend& backend)
    {
        QMetaObject::invokeMethod(backend.m_metis, [metis = backend.m_metis] {
            metis->clearCwKeying();
        }, Qt::BlockingQueuedConnection);
    }
    static std::chrono::steady_clock::time_point upOnAir(const Hl2Backend& backend)
    {
        return backend.m_metis->cwScheduledUpOnAir();
    }
};
}  // namespace AetherSDR::hl2

using namespace AetherSDR;
using namespace AetherSDR::hl2;
using Clock = std::chrono::steady_clock;
using Access = MetisClientTestAccess;

static int g_checks = 0;
static int g_failures = 0;
static void check(bool ok, const std::string& what)
{
    ++g_checks;
    std::fprintf(ok ? stdout : stderr, "%s: %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) {
        ++g_failures;
    }
}

constexpr std::int64_t kRate = Access::rateHz();
constexpr std::int64_t kFrame = kTxSamplesPerPacket;
static std::int64_t samplesOfMs(std::int64_t ms) { return ms * kRate / 1000; }

// The I channel of every sample in one packet, in wire order.
static void appendI(const std::array<std::uint8_t, kUsbPacketSize>& pkt, std::vector<int>& out)
{
    constexpr std::size_t kSamplesPerFrame = static_cast<std::size_t>(kTxSamplesPerPacket) / 2;
    constexpr std::size_t kSampleBytes = kFramePayload / kSamplesPerFrame;
    for (const std::size_t fs : {std::size_t{8}, std::size_t{8} + kFrameSize}) {
        const std::uint8_t* payload = pkt.data() + fs + 8;
        for (std::size_t k = 0; k < kSamplesPerFrame; ++k) {
            const auto hi = payload[k * kSampleBytes + 4];
            const auto lo = payload[k * kSampleBytes + 5];
            out.push_back(static_cast<std::int16_t>(static_cast<std::uint16_t>((hi << 8) | lo)));
        }
    }
}

struct Run { std::int64_t start; std::int64_t length; };
// Carrier = above half the plateau. The 5 ms rise and fall are symmetric, so a
// run's length at the half crossing equals the distance between its edges.
static std::vector<Run> carriers(const std::vector<int>& iTrace)
{
    const int half = static_cast<int>(kHl2CarrierAmplitude * 32767.0 / 2.0);
    std::vector<Run> runs;
    std::int64_t start = -1;
    for (std::size_t n = 0; n < iTrace.size(); ++n) {
        const bool on = iTrace[n] > half;
        if (on && start < 0) {
            start = static_cast<std::int64_t>(n);
        } else if (!on && start >= 0) {
            runs.push_back({start, static_cast<std::int64_t>(n) - start});
            start = -1;
        }
    }
    if (start >= 0) {
        runs.push_back({start, static_cast<std::int64_t>(iTrace.size()) - start});
    }
    return runs;
}

struct Edge {
    std::int64_t ms;        // scheduled instant after the anchor
    bool down;
    std::int64_t arrivalMs; // when the call reaches the I/O thread
    bool scheduled = true;
    std::int64_t releaseMs = -1;   // a key-down's own key-up instant; -1 none
};

struct Bench {
    TxTestAuthority tx;
    MetisClient client;
    Clock::time_point t0 = Clock::now();
    std::vector<int> iTrace;
    Bench()
    {
        Access::setStreaming(client);
        client.enableTransmit(true);
        client.setMox(true, tx.operation);
        Access::anchor(client, t0);
    }
    // Build frames until `endMs`, delivering each edge before the first frame
    // whose first sample is at or past its arrival.
    void play(const std::vector<Edge>& edges, std::int64_t endMs,
              const TxCoordinator::Operation* operation = nullptr)
    {
        std::size_t next = 0;
        while (Access::frameSample(client) < samplesOfMs(endMs)) {
            while (next < edges.size()
                   && samplesOfMs(edges[next].arrivalMs) <= Access::frameSample(client)) {
                const Edge& e = edges[next++];
                client.setCwKeyDown(e.down, operation ? *operation : tx.operation,
                                    e.scheduled ? t0 + std::chrono::milliseconds(e.ms) : Clock::time_point{},
                                    e.releaseMs >= 0 ? t0 + std::chrono::milliseconds(e.releaseMs)
                                                     : Clock::time_point{});
            }
            appendI(client.buildNextControlPacket(), iTrace);
        }
    }
};

// 30 WPM, a 40 ms unit: two dit-dah-dit characters, the second 400 ms later.
struct Element { std::int64_t downMs; std::int64_t upMs; };
static std::vector<Element> train()
{
    std::vector<Element> els;
    for (const std::int64_t base : {100, 500}) {
        els.push_back({base, base + 40});
        els.push_back({base + 80, base + 200});
        els.push_back({base + 240, base + 280});
    }
    return els;
}

static std::vector<Edge> edgesOf(const std::vector<Element>& els,
                                 const std::function<std::int64_t(std::int64_t, bool)>& arrival,
                                 bool withRelease)
{
    std::vector<Edge> edges;
    for (const Element& el : els) {
        edges.push_back({el.downMs, true, arrival(el.downMs, true), true, withRelease ? el.upMs : -1});
        edges.push_back({el.upMs, false, arrival(el.upMs, false)});
    }
    return edges;
}

static bool within(std::int64_t got, std::int64_t want, std::int64_t tol) { return got >= want - tol && got <= want + tol; }

static void expectExactTrain(const char* label, const std::function<std::int64_t(std::int64_t, bool)>& arrival,
                             bool withRelease)
{
    const std::string name = std::string(label) + (withRelease ? " (with release)" : " (key-up hops)");
    Bench b;
    const auto els = train();
    b.play(edgesOf(els, arrival, withRelease), 1000);
    const auto runs = carriers(b.iTrace);
    check(runs.size() == els.size(), name + ": every element is on the wire");
    bool lengths = runs.size() == els.size();
    bool spaces = lengths;
    bool placed = lengths;
    for (std::size_t k = 0; lengths && k < els.size(); ++k) {
        lengths &= within(runs[k].length, samplesOfMs(els[k].upMs - els[k].downMs), 1);
        if (k + 1 < els.size()) {
            const std::int64_t space = runs[k + 1].start - (runs[k].start + runs[k].length);
            spaces &= within(space, samplesOfMs(els[k + 1].downMs - els[k].upMs), 1);
        }
        // Rise starts at the scheduled sample plus the lead; the half crossing
        // is half the rise later, the same for every element.
        placed &= runs[k].start - (samplesOfMs(els[k].downMs) + Access::leadSamples())
               == runs[0].start - (samplesOfMs(els[0].downMs) + Access::leadSamples());
    }
    check(lengths, name + ": every element within +-1 sample of nominal");
    check(spaces, name + ": every space within +-1 sample of nominal");
    check(placed, name + ": every element starts at its scheduled sample plus the lead");
    check(b.client.cwShiftedElements() == 0 && b.client.cwRefusedDowns() == 0 && b.client.cwShiftSamples() == 0,
          name + ": nothing shifted, nothing refused");
}

static void testScheduledTrainsAreSampleExact()
{
    const std::int64_t leadMs = Access::leadSamples() * 1000 / kRate;
    check(15 + kFrame * 1000 / kRate + 1 < leadMs, "15 ms of lateness is inside the lead minus one frame");
    for (const bool withRelease : {true, false}) {
        expectExactTrain("on time", [](std::int64_t ms, bool) { return ms; }, withRelease);
        // Arrival a fixed 15 ms after the instant (< lead, and < lead minus one
        // frame, so the frame quantisation of delivery cannot push it past).
        expectExactTrain("15 ms late", [](std::int64_t ms, bool) { return ms + 15; }, withRelease);
        // A 15 ms stall every 50 ms: edges inside a stall arrive together at its end.
        expectExactTrain("15 ms stall every 50 ms", [](std::int64_t ms, bool) {
            const std::int64_t phase = ms % 50;
            return phase < 15 ? ms - phase + 15 : ms;
        }, withRelease);
    }
}

// What a shifted over must look like on the wire: every element at its exact
// length, no element missing, no space shorter than nominal, and the spaces'
// total excess equal to the shift the over accumulated after its first element.
struct ShiftVerdict {
    bool allSent = false;
    bool lengths = false;
    bool spacesNeverShort = false;
    bool excessIsShift = false;
    bool placementNeverEarlier = false;
    std::int64_t excess = 0;
    std::size_t stretchedSpaces = 0;
};
static ShiftVerdict judgeShifted(const Bench& b, const std::vector<Element>& els)
{
    ShiftVerdict v;
    const auto runs = carriers(b.iTrace);
    v.allSent = runs.size() == els.size();
    if (!v.allSent) {
        return v;
    }
    v.lengths = v.spacesNeverShort = v.placementNeverEarlier = true;
    std::int64_t previousOffset = std::numeric_limits<std::int64_t>::min() / 2;   // no underflow at "- 1"
    for (std::size_t k = 0; k < els.size(); ++k) {
        v.lengths &= within(runs[k].length, samplesOfMs(els[k].upMs - els[k].downMs), 1);
        const std::int64_t offset = runs[k].start - (samplesOfMs(els[k].downMs) + Access::leadSamples());
        v.placementNeverEarlier &= offset >= previousOffset - 1;
        previousOffset = offset;
        if (k + 1 < els.size()) {
            const std::int64_t space = runs[k + 1].start - (runs[k].start + runs[k].length);
            const std::int64_t nominal = samplesOfMs(els[k + 1].downMs - els[k].upMs);
            v.spacesNeverShort &= space >= nominal - 1;
            v.excess += space - nominal;
            v.stretchedSpaces += space - nominal > 1 ? 1 : 0;
        }
    }
    const std::int64_t first = runs[0].start - (samplesOfMs(els[0].downMs) + Access::leadSamples());
    const std::int64_t last = runs.back().start - (samplesOfMs(els.back().downMs) + Access::leadSamples());
    v.excessIsShift = within(v.excess, last - first, static_cast<std::int64_t>(els.size()));
    return v;
}

static void testLateKeyDownShiftsWholeElement()
{
    const std::int64_t leadMs = Access::leadSamples() * 1000 / kRate;
    const std::int64_t lateMs = leadMs + 10;
    const std::string late = std::to_string(lateMs) + " ms late";
    check(lateMs > leadMs + kFrame * 1000 / kRate, late + ": past the lead plus one frame");
    {
        Bench b;
        const auto els = train();
        b.play(edgesOf(els, [lateMs](std::int64_t ms, bool) { return ms + lateMs; }, true), 1000);
        const ShiftVerdict v = judgeShifted(b, els);
        check(v.allSent, late + ": every element is on the wire, none dropped");
        check(v.lengths, late + ": every element within +-1 sample of nominal");
        check(v.spacesNeverShort && v.excessIsShift,
              late + ": no space shorter than nominal; the spaces' excess is the shift grown after element 1");
        check(v.stretchedSpaces + 1 <= b.client.cwShiftedElements(),
              late + ": a space is longer only where a key-down shifted the over (counted)");
        check(v.placementNeverEarlier, late + ": no element moves earlier than the one before it");
        check(b.client.cwShiftSamples() >= samplesOfMs(lateMs - leadMs)
                  && b.client.cwShiftSamples() <= samplesOfMs(lateMs - leadMs) + kFrame,
              late + ": the over's shift is the lateness past the lead, within one frame");
        check(b.client.cwRefusedDowns() == 0 && b.client.cwLateKeyUps() == 0 && !b.client.cwKeyDown(),
              late + ": nothing refused, no late key-up, the key is up at the end");
    }
    {
        // Every second element arrives late past the lead; the others on time.
        Bench b;
        const auto els = train();
        std::vector<Edge> edges;
        for (std::size_t k = 0; k < els.size(); ++k) {
            const std::int64_t late = (k % 2) ? lateMs : 0;
            edges.push_back({els[k].downMs, true, els[k].downMs + late, true, els[k].upMs});
            edges.push_back({els[k].upMs, false, els[k].upMs + late});
        }
        b.play(edges, 1000);
        const ShiftVerdict v = judgeShifted(b, els);
        check(v.allSent && v.lengths, "mixed lateness: every element sent, each within +-1 sample");
        check(v.spacesNeverShort && v.excessIsShift && v.stretchedSpaces <= b.client.cwShiftedElements(),
              "mixed lateness: spaces never short; only the shifts lengthen one");
        check(b.client.cwShiftedElements() >= 1 && b.client.cwRefusedDowns() == 0,
              "mixed lateness: the shift is counted, nothing refused");
    }
    {
        // Key-ups late past the lead through the hop, but each down carried its release:
        // the late key-up is bookkeeping and the element is exact.
        Bench b;
        const auto els = train();
        std::vector<Edge> edges;
        for (const Element& el : els) {
            edges.push_back({el.downMs, true, el.downMs, true, el.upMs});
            edges.push_back({el.upMs, false, el.upMs + lateMs});
        }
        b.play(edges, 1000);
        const auto runs = carriers(b.iTrace);
        bool exact = runs.size() == els.size();
        for (std::size_t k = 0; exact && k < runs.size(); ++k) {
            exact &= within(runs[k].length, samplesOfMs(els[k].upMs - els[k].downMs), 1);
        }
        check(exact, "late key-up with release: every element within +-1 sample of nominal");
        check(b.client.cwLateKeyUps() == 0 && b.client.cwShiftedElements() == 0,
              "late key-up with release: no late key-up applied, no shift");
    }
    {
        // Without a release (an older producer), a late key-up is applied at
        // once: the element runs long, never short, and it is counted.
        Bench b;
        const auto els = train();
        std::vector<Edge> edges;
        for (const Element& el : els) {
            edges.push_back({el.downMs, true, el.downMs});
            edges.push_back({el.upMs, false, el.upMs + lateMs});
        }
        b.play(edges, 1000);
        const auto runs = carriers(b.iTrace);
        bool longNotShort = runs.size() == els.size();
        for (std::size_t k = 0; longNotShort && k < runs.size(); ++k) {
            const std::int64_t nominal = samplesOfMs(els[k].upMs - els[k].downMs);
            longNotShort &= runs[k].length >= nominal
                && runs[k].length <= nominal + samplesOfMs(lateMs) - Access::leadSamples() + kFrame + 1;
        }
        check(longNotShort, "late key-up, no release: applied at once, longer by at most the excess plus a frame");
        check(b.client.cwLateKeyUps() == els.size() && !b.client.cwKeyDown(),
              "late key-up, no release: every one counted, the key is up at the end");
    }
    {
        // An earlier key-up than the release (a keyer stopped mid-element) wins.
        Bench b;
        b.play({{100, true, 100, true, 220}, {160, false, 160}}, 400);
        const auto runs = carriers(b.iTrace);
        check(runs.size() == 1 && within(runs[0].length, samplesOfMs(60), 1),
              "an earlier key-up replaces the scheduled release: the element ends at the earlier instant");
    }
}

static void testShiftBoundRefusesTheOver()
{
    const std::int64_t leadMs = Access::leadSamples() * 1000 / kRate;
    const std::int64_t boundMs = Access::maxShiftSamples() * 1000 / kRate;
    {
        // One element on time, then a key-down later than lead + bound: refused,
        // and the rest of the over with it, even edges that arrive on time.
        Bench b;
        b.play({{100, true, 100, true, 140}, {140, false, 140},
                {180, true, 180 + leadMs + boundMs + 20, true, 220}, {220, false, 220 + leadMs + boundMs + 20},
                {600, true, 600, true, 640}, {640, false, 640}}, 1000);
        const auto runs = carriers(b.iTrace);
        check(runs.size() == 1 && within(runs[0].length, samplesOfMs(40), 1),
              "past the bound: the on-time element is sent whole, the late one and the rest of the over are not");
        check(b.client.cwRefusedDowns() == 2 && !b.client.cwKeyDown() && Access::pending(b.client) == 0,
              "past the bound: both refused downs are counted, the key is up, nothing pending");
        b.client.clearCwKeying();   // the over ends (break-in hang, MOX off)
        b.client.setMox(true, b.tx.operation);
        check(b.client.cwShiftSamples() == 0, "the over's end clears the shift");
        const std::int64_t nowMs = Access::frameSample(b.client) * 1000 / kRate;
        b.play({{nowMs + 50, true, nowMs + 50, true, nowMs + 90}, {nowMs + 90, false, nowMs + 90}}, nowMs + 300);
        const auto after = carriers(b.iTrace);
        check(after.size() == 2 && within(after[1].length, samplesOfMs(40), 1),
              "the next over keys again after a refused one");
    }
    {
        // Just inside the bound: shifted, sent whole.
        Bench b;
        const std::int64_t late = leadMs + boundMs - 10;
        b.play({{100, true, 100 + late, true, 140}, {140, false, 140 + late}}, 1000);
        const auto runs = carriers(b.iTrace);
        check(runs.size() == 1 && within(runs[0].length, samplesOfMs(40), 1) && b.client.cwRefusedDowns() == 0,
              "inside the bound: a key-down " + std::to_string(late) + " ms late is shifted and sent whole");
    }
}

static void testForcedKeyUpCap()
{
    const std::int64_t capMs = Access::capSamples() * 1000 / kRate;
    {
        Bench b;
        b.play({{100, true, 100}}, 100 + capMs + 200);
        const auto runs = carriers(b.iTrace);
        check(runs.size() == 1 && within(runs[0].length, Access::capSamples(), 1),
              "a scheduled down with no up is forced up after the element cap");
        check(b.client.cwForcedKeyUps() == 1, "the forced key-up is counted");
    }
    {
        Bench b;
        b.play({{100, true, 100, false}}, 100 + capMs + 200);
        const auto runs = carriers(b.iTrace);
        check(runs.size() == 1 && runs[0].start + runs[0].length == static_cast<std::int64_t>(b.iTrace.size()),
              "an unscheduled (straight-key) hold is not capped");
        check(b.client.cwForcedKeyUps() == 0, "no forced key-up for an unscheduled hold");
    }
}

static void testScheduleCapacity()
{
    {
        Bench b;
        for (std::size_t k = 0; k < Access::capacity(); ++k) {
            b.client.setCwKeyDown(k % 2 == 0, b.tx.operation, b.t0 + std::chrono::milliseconds(100 + 10 * k));
        }
        check(Access::pending(b.client) == Access::capacity() && b.client.cwScheduleOverflows() == 0,
              "a full schedule holds its capacity without overflow");
    }
    {
        Bench b;
        for (std::size_t k = 0; k <= Access::capacity(); ++k) {
            b.client.setCwKeyDown(k % 2 == 0, b.tx.operation, b.t0 + std::chrono::milliseconds(100 + 10 * k));
        }
        check(b.client.cwScheduleOverflows() == 1 && Access::pending(b.client) == 0 && !b.client.cwKeyDown(),
              "one edge past capacity flushes the schedule and keys up, counted");
        b.play({}, 500);
        check(carriers(b.iTrace).empty(), "a flushed schedule puts no carrier on the wire");
    }
}

// A scheduled down pending, then `act`, then frames past its target.
static void expectFlushed(const char* label, const std::function<void(Bench&)>& act)
{
    Bench b;
    b.client.setCwKeyDown(true, b.tx.operation, b.t0 + std::chrono::milliseconds(100));
    b.client.setCwKeyDown(false, b.tx.operation, b.t0 + std::chrono::milliseconds(200));
    check(Access::pending(b.client) == 2, std::string(label) + ": two edges pending before");
    act(b);
    check(Access::pending(b.client) == 0, std::string(label) + ": the schedule is empty");
    b.play({}, 400);
    check(carriers(b.iTrace).empty(), std::string(label) + ": no carrier from the flushed edges");
}

static void testFlushPaths()
{
    expectFlushed("clearCwKeying", [](Bench& b) { b.client.clearCwKeying(); });
    expectFlushed("stop", [](Bench& b) {
        b.client.stop();
        Access::setStreaming(b.client);
        b.client.setMox(true, b.tx.operation);
    });
    expectFlushed("transmit gate closed and reopened", [](Bench& b) {
        b.client.enableTransmit(false);
        b.client.enableTransmit(true);
        b.client.setMox(true, b.tx.operation);
    });
    expectFlushed("unscheduled key-up", [](Bench& b) { b.client.setCwKeyDown(false, b.tx.operation); });
    {
        // A pending down is re-checked against its own operation when due.
        Bench b;
        const auto producer = b.tx.coordinator.registerProducer();
        const auto element = producer.request();
        const auto intent = b.tx.coordinator.beginRequest(element, b.tx.operation, TxCoordinator::Activity::CwKey);
        const auto op = b.tx.coordinator.requestOperation(element);
        b.client.setCwKeyDown(true, op, b.t0 + std::chrono::milliseconds(100));
        check(Access::pending(b.client) == 1, "ended operation: the down is pending");
        (void)b.tx.coordinator.endIntent(intent);
        b.play({}, 400);
        check(carriers(b.iTrace).empty() && !b.client.cwKeyDown(),
              "ended operation: a pending down whose operation ended is not keyed");
    }
    {
        // CW mode stays up on a later, live element, so only the due-time
        // re-check stands between the ended element and the carrier.
        Bench b;
        const auto producer = b.tx.coordinator.registerProducer();
        const auto element = producer.request();
        const auto intent = b.tx.coordinator.beginRequest(element, b.tx.operation, TxCoordinator::Activity::CwKey);
        const auto op = b.tx.coordinator.requestOperation(element);
        b.client.setCwKeyDown(true, op, b.t0 + std::chrono::milliseconds(100),
                              b.t0 + std::chrono::milliseconds(140));
        b.client.setCwKeyDown(true, b.tx.operation, b.t0 + std::chrono::milliseconds(300),
                              b.t0 + std::chrono::milliseconds(340));
        (void)b.tx.coordinator.endIntent(intent);
        b.play({}, 500);
        const auto liveStart = static_cast<std::size_t>(samplesOfMs(300) + Access::leadSamples());
        const bool silent = b.iTrace.size() > liveStart
            && std::all_of(b.iTrace.begin(), b.iTrace.begin() + static_cast<std::ptrdiff_t>(liveStart),
                           [](int s) { return s == 0; });
        check(silent, "ended operation, MOX live: no CW sample before the live element (no carrier)");
        const auto runs = carriers(b.iTrace);
        check(runs.size() == 1 && within(runs[0].length, samplesOfMs(40), 1),
              "ended operation, MOX live: the live element after it is keyed whole");
    }
}

static void testUnscheduledPathUnchanged()
{
    {
        Bench b;
        const std::int64_t downFrame = 10;
        const std::int64_t upFrame = 30;
        while (Access::frameSample(b.client) < downFrame * kFrame) {
            appendI(b.client.buildNextControlPacket(), b.iTrace);
        }
        b.client.setCwKeyDown(true, b.tx.operation);
        while (Access::frameSample(b.client) < upFrame * kFrame) {
            appendI(b.client.buildNextControlPacket(), b.iTrace);
        }
        b.client.setCwKeyDown(false, b.tx.operation);
        b.play({}, 1000);
        const auto runs = carriers(b.iTrace);
        check(runs.size() == 1 && within(runs[0].length, (upFrame - downFrame) * kFrame, 1),
              "unscheduled: the edge takes effect at the next frame built, as before");
        check(Access::pending(b.client) == 0, "unscheduled: nothing is scheduled");
    }
    {
        // The defect #5065 names, on the unscheduled path: down and up landing
        // before the same frame leave no element at all.
        Bench b;
        b.client.setCwKeyDown(true, b.tx.operation);
        b.client.setCwKeyDown(false, b.tx.operation);
        b.play({}, 200);
        check(carriers(b.iTrace).empty(), "unscheduled: a down and up in one frame are lost (the defect)");
    }
}

// Level 2, backend half: Hl2Backend's own I/O thread, socket-free. The edge's
// instant must cross Hl2Backend::requestCwControl into MetisClient, and the
// break-in hang must count from the key-up's on-air sample.
static void testBackendCarriesScheduledAt()
{
    TxTestAuthority tx;
    Hl2Backend backend;
    const Clock::time_point t0 = Clock::now();
    Hl2TxGateTestAccess::prepare(backend, t0);
    const int delayMs = 50;
    const auto edge = [&](bool down, std::int64_t ms) {
        const CwControlRequest request{CwControlRequest::Kind::Carrier, down, true, delayMs,
                                       t0 + std::chrono::milliseconds(ms), QStringLiteral("test"), 0, 0};
        return backend.requestCwControl(request, TxCoordinator::Command{tx.operation, down, {}});
    };
    std::vector<int> iTrace;
    const auto buildTo = [&](std::int64_t ms) {
        while (static_cast<std::int64_t>(iTrace.size()) < samplesOfMs(ms)) {
            appendI(Hl2TxGateTestAccess::packet(backend), iTrace);
        }
    };
    check(edge(true, 100) == ReceiveDispatch::Dispatched, "backend: a scheduled down is dispatched");
    check(Hl2TxGateTestAccess::pending(backend) == 1, "backend: the down's instant reaches MetisClient's schedule");
    buildTo(155);   // the up arrives 15 ms late
    const Clock::time_point before = Clock::now();
    check(edge(false, 140) == ReceiveDispatch::Dispatched, "backend: a scheduled up is dispatched");
    const Clock::time_point after = Clock::now();
    const auto onAir = t0 + std::chrono::milliseconds(140) + std::chrono::milliseconds(MetisClient::kCwEdgeLeadMs);
    const auto aheadMs = [&](Clock::time_point now) {
        const auto d = onAir - now;
        return d > Clock::duration::zero()
            ? static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(d).count()) : 0;
    };
    check(Hl2TxGateTestAccess::hangActive(backend) && Hl2TxGateTestAccess::hangDelayMs(backend) == delayMs,
          "backend: the break-in hang runs with the configured delay");
    check(Hl2TxGateTestAccess::hangIntervalMs(backend) >= delayMs + aheadMs(after)
              && Hl2TxGateTestAccess::hangIntervalMs(backend) <= delayMs + aheadMs(before),
          "backend: the hang counts from the key-up's on-air instant");
    buildTo(400);
    const auto runs = carriers(iTrace);
    check(runs.size() == 1 && within(runs[0].length, samplesOfMs(40), 1),
          "backend: a 40 ms element delivered 15 ms late is 40 ms on the wire, +-1 sample");
    backend.setCwKeying(false, true, delayMs, tx.operation);
    check(Hl2TxGateTestAccess::hangIntervalMs(backend) == delayMs,
          "backend: an unscheduled key-up's hang is the configured delay alone");
}

// The break-in hang must count from the SHIFTED key-up's wire instant: MOX may
// not fall before a shifted element (and its 5 ms fall) has gone out.
static void testBackendHangWaitsForTheShiftedKeyUp()
{
    TxTestAuthority tx;
    Hl2Backend backend;
    const Clock::time_point t0 = Clock::now();
    Hl2TxGateTestAccess::prepare(backend, t0);
    const int delayMs = 20;
    const std::int64_t lateMs = 120;
    const std::int64_t leadMs = MetisClient::kCwEdgeLeadMs;
    const auto edge = [&](bool down, std::int64_t ms, std::int64_t releaseMs) {
        CwControlRequest request{CwControlRequest::Kind::Carrier, down, true, delayMs,
                                 t0 + std::chrono::milliseconds(ms), QStringLiteral("test"), 0, 0};
        if (releaseMs >= 0) {
            request.releaseAt = t0 + std::chrono::milliseconds(releaseMs);
        }
        return backend.requestCwControl(request, TxCoordinator::Command{tx.operation, down, {}});
    };
    std::vector<int> iTrace;
    while (static_cast<std::int64_t>(iTrace.size()) < samplesOfMs(100 + lateMs)) {
        appendI(Hl2TxGateTestAccess::packet(backend), iTrace);   // the frame cursor runs ahead: the down is late
    }
    (void)edge(true, 100, 140);
    (void)edge(false, 140, -1);
    const std::int64_t shift = Hl2TxGateTestAccess::shiftSamples(backend);
    check(shift >= samplesOfMs(lateMs - leadMs) && shift <= samplesOfMs(lateMs - leadMs) + kFrame,
          "backend: a key-down 120 ms late shifts the over by the lateness past the lead");
    const auto expectOnAir = t0 + std::chrono::milliseconds(140 + leadMs)
        + std::chrono::nanoseconds(shift * 1'000'000'000LL / kRate);
    const auto onAir = Hl2TxGateTestAccess::upOnAir(backend);
    check(onAir > expectOnAir - std::chrono::milliseconds(1) && onAir < expectOnAir + std::chrono::milliseconds(1),
          "backend: the shifted key-up's wire instant is published");
    const auto waitUntil = [](Clock::time_point t) {
        QEventLoop loop;
        const auto ms = std::chrono::ceil<std::chrono::milliseconds>(t - Clock::now()).count();
        QTimer::singleShot(static_cast<int>(std::max<std::int64_t>(0, ms)), &loop, &QEventLoop::quit);
        loop.exec();
    };
    // The unshifted hang would end at t0 + 140 + lead + delay; the shifted one ~100 ms later.
    waitUntil(t0 + std::chrono::milliseconds(140 + leadMs + delayMs + 50));
    check(Hl2TxGateTestAccess::cwAutoKeyed(backend),
          "backend: the hang has not ended at the unshifted key-up's instant plus the delay");
    waitUntil(onAir + std::chrono::milliseconds(delayMs + 150));
    check(!Hl2TxGateTestAccess::cwAutoKeyed(backend),
          "backend: the hang ends after the shifted key-up's wire instant plus the delay");
}

// #5065 review 1: a keyer stopped mid-element sends its key-up with the
// element's deadline as scheduledAt (as on main, for every other backend) and
// the stop instant in releaseAt. The HL2 keys up at the stop; without it, at
// the deadline. 5 WPM: a 720 ms dah, stopped 60 ms in.
static void testBackendKeysUpAtAKeyerStop()
{
    for (const bool stopped : {true, false}) {
        TxTestAuthority tx;
        Hl2Backend backend;
        const Clock::time_point t0 = Clock::now();
        Hl2TxGateTestAccess::prepare(backend, t0);
        const auto edge = [&](bool down, std::int64_t ms, std::int64_t releaseMs) {
            CwControlRequest request{CwControlRequest::Kind::Carrier, down, true, 20,
                                     t0 + std::chrono::milliseconds(ms), QStringLiteral("test"), 0, 0};
            if (releaseMs >= 0) {
                request.releaseAt = t0 + std::chrono::milliseconds(releaseMs);
            }
            return backend.requestCwControl(request, TxCoordinator::Command{tx.operation, down, {}});
        };
        (void)edge(true, 100, 820);
        (void)edge(false, 820, stopped ? 160 : -1);
        std::vector<int> iTrace;
        while (static_cast<std::int64_t>(iTrace.size()) < samplesOfMs(1100)) {
            appendI(Hl2TxGateTestAccess::packet(backend), iTrace);
        }
        const auto runs = carriers(iTrace);
        if (stopped) {
            check(runs.size() == 1 && within(runs[0].length, samplesOfMs(60), 1),
                  "backend: a key-up carrying a keyer stop ends the element at the stop");
        } else {
            check(runs.size() == 1 && within(runs[0].length, samplesOfMs(720), 1),
                  "backend: a key-up without a stop instant ends the element at its release");
        }
    }
}

// #5065: a refused over raises ONE "TX failed" notice, on the model's thread,
// whatever number of key-downs the over goes on to refuse; the next refused
// over raises a new one. MOX is not touched by it.
static void testRefusedOverRaisesOneNotice()
{
    TxTestAuthority tx;
    RadioModel model;
    model.setBackendForTest(std::make_unique<Hl2Backend>(), QStringLiteral("hl2"));
    auto* backend = static_cast<Hl2Backend*>(model.backend());
    const Clock::time_point t0 = Clock::now();
    Hl2TxGateTestAccess::prepare(*backend, t0);
    QStringList keys;
    QStringList texts;
    bool onModelThread = true;
    QObject::connect(&model, &RadioModel::interlockNotificationRequested, &model,
                     [&](const QString& message, const QString& key, const QString&) {
        onModelThread = onModelThread && QThread::currentThread() == model.thread();
        if (key.startsWith(QStringLiteral("tx-failed:"))) {
            keys << key;
            texts << message;
        }
    });
    const auto down = [&](std::int64_t ms) {
        CwControlRequest request{CwControlRequest::Kind::Carrier, true, true, 20,
                                 t0 + std::chrono::milliseconds(ms), QStringLiteral("test"), 0, 0};
        request.releaseAt = t0 + std::chrono::milliseconds(ms + 40);
        (void)backend->requestCwControl(request, TxCoordinator::Command{tx.operation, true, {}});
    };
    std::int64_t builtMs = 0;
    const auto buildTo = [&](std::int64_t ms) {
        while (builtMs < ms) {
            (void)Hl2TxGateTestAccess::packet(*backend);
            builtMs = Hl2TxGateTestAccess::frameMs(*backend);
        }
    };
    // The I/O thread's signal, then two queued hops (backend, model).
    const auto settle = [] {
        QElapsedTimer waited;
        waited.start();
        while (waited.elapsed() < 200) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
    };
    // The frame cursor 600 ms on: a key-down at 100 ms is 460 ms past its
    // sample and the lead, over the 250 ms bound.
    buildTo(600);
    down(100);
    settle();
    check(keys.size() == 1, "one refused key-down: one notice");
    check(texts.value(0) == QStringLiteral("CW keying fell too far behind"),
          "the notice reads \"CW keying fell too far behind\"");
    down(150);
    down(200);
    down(250);
    settle();
    check(keys.size() == 1 && Hl2TxGateTestAccess::refusedDowns(*backend) == 4,
          "four refused key-downs in one over: still one notice");
    Hl2TxGateTestAccess::endOver(*backend);
    buildTo(1400);
    down(900);
    down(950);
    settle();
    check(keys.size() == 2 && keys[0] != keys[1], "the next refused over: a new notice");
    check(onModelThread, "every notice is raised on the model's thread");
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-cw-edge-timing"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "Cannot create isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    testScheduledTrainsAreSampleExact();
    testLateKeyDownShiftsWholeElement();
    testShiftBoundRefusesTheOver();
    testForcedKeyUpCap();
    testScheduleCapacity();
    testFlushPaths();
    testUnscheduledPathUnchanged();
    testBackendCarriesScheduledAt();
    testBackendHangWaitsForTheShiftedKeyUp();
    testBackendKeysUpAtAKeyerStop();
    testRefusedOverRaisesOneNotice();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
