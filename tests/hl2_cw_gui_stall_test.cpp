// #5065, the issue's own case: a real IambicKeyer on its own thread, its edges
// hopping to the main (GUI) thread before Hl2Backend::requestCwControl, as
// MainWindow does, while the main thread is blocked for kStallMs every
// kStallPeriodMs. Frames are built on the backend's I/O thread at the wall-clock
// EP2 rate by a third thread, so a GUI stall reaches the wire only through
// the edges' arrival. Socket-free; nothing is keyed.
//
// The stall is LONGER than the lead (kCwEdgeLeadMs), 70 ms every 110 ms during
// a 30 WPM squeeze, so key-downs arrive late and the over must shift. Each key-down carries its own release
// (the keyer's key-up instant), and a late key-down shifts the rest of the over
// instead of being dropped. Expectation: every element on the wire, every
// element's length equal to the keyer's scheduled gap +-1 sample, no space
// shorter than scheduled, and a space longer only where a shift happened, by
// exactly the shift's growth.
//
// Wall-clock: an attempt counts only when at least one edge reached the GUI
// thread later than the lead plus one frame (so a shift had to happen) and none
// later than the shift bound. That precondition is measured before the code
// under test runs. Three attempts that miss it end in ctest "Skipped" (77),
// never in a pass.

#include "TestSettingsProfile.h"
#include "TxTestAuthority.h"
#include "core/IambicKeyer.h"
#include "core/backends/CwControlRequest.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QString>
#include <QTimer>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace AetherSDR::hl2 {
struct MetisClientTestAccess {
    static void setStreaming(MetisClient& client) { client.m_running = true; }
    static void anchor(MetisClient& client, std::chrono::steady_clock::time_point t)
    {
        client.m_cwAnchor = t;
        client.m_cwFrameSample = 0;
    }
    static constexpr int rateHz() { return MetisClient::kEp2AudioRateHz; }
    static constexpr std::int64_t maxShiftSamples() { return MetisClient::kCwMaxEdgeShiftSamples; }
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
    struct Totals { std::int64_t shift; std::uint64_t shifted, refused, lateUps; };
    static Totals totals(Hl2Backend& backend)
    {
        Totals t{};
        QMetaObject::invokeMethod(backend.m_metis, [&t, metis = backend.m_metis] {
            t = {metis->cwShiftSamples(), metis->cwShiftedElements(), metis->cwRefusedDowns(),
                 metis->cwLateKeyUps()};
        }, Qt::BlockingQueuedConnection);
        return t;
    }
    static std::array<std::uint8_t, kUsbPacketSize> packet(Hl2Backend& backend)
    {
        std::array<std::uint8_t, kUsbPacketSize> packet{};
        QMetaObject::invokeMethod(backend.m_metis, [&packet, metis = backend.m_metis] {
            packet = metis->buildNextControlPacket();
        }, Qt::BlockingQueuedConnection);
        return packet;
    }
};
}  // namespace AetherSDR::hl2

using namespace AetherSDR;
using namespace AetherSDR::hl2;
using Clock = std::chrono::steady_clock;

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

constexpr std::int64_t kRate = MetisClientTestAccess::rateHz();
constexpr std::int64_t kFrame = kTxSamplesPerPacket;
constexpr std::int64_t kLeadSamples = static_cast<std::int64_t>(MetisClient::kCwEdgeLeadMs) * kRate / 1000;
constexpr int kWpm = 30;                 // 40 ms unit
constexpr int kStallMs = 70;             // longer than the lead plus one frame: edges arrive late
constexpr int kStallPeriodMs = 110;      // not a multiple of the unit: stalls land on edges unevenly
constexpr int kHoldMs = 1500;            // both paddles held: a dit-dah squeeze, about 23 elements
constexpr int kAttempts = 3;

static std::int64_t sampleAt(Clock::time_point t0, Clock::time_point t)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t - t0).count() * kRate / 1'000'000'000LL;
}

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

struct EdgeSeen {
    bool down;
    Clock::time_point when;      // the keyer's scheduled instant
    Clock::time_point releaseAt; // a key-down's own key-up instant
    Clock::time_point arrival;   // when the GUI thread got to it
};

struct Attempt {
    bool valid = false;
    std::string why;
    std::size_t elements = 0;
    std::size_t onWire = 0;
    bool releasePaired = true;           // each down's releaseAt is its up's instant
    std::int64_t worstLengthErr = 0;     // samples, |element on wire - scheduled|
    std::int64_t worstSpaceShort = 0;    // samples, how much shorter than scheduled a space got (<= 0 good)
    std::int64_t spaceExcess = 0;        // samples, total of (space on wire - scheduled)
    std::size_t stretchedSpaces = 0;     // spaces more than 1 sample longer than scheduled
    std::int64_t offsetGrowth = 0;       // samples, last element's offset minus the first's
    bool placementMonotone = true;       // no element placed earlier relative to its instant than the one before
    Hl2TxGateTestAccess::Totals totals{};
    int maxLateMs = 0;
    double medianLateMs = 0.0;
};

static Attempt runOnce()
{
    Attempt result;
    TxTestAuthority tx;
    Hl2Backend backend;
    const Clock::time_point t0 = Clock::now();
    Hl2TxGateTestAccess::prepare(backend, t0);

    QObject gui;   // the main thread's stand-in for RadioModel
    std::mutex edgesMu;
    std::vector<EdgeSeen> edges;

    IambicKeyer keyer;
    keyer.setMode(IambicKeyer::Mode::IambicA);
    keyer.setWpm(kWpm);
    keyer.setOnRoutedKeyDownChange([&](bool down, Clock::time_point when, Clock::time_point releaseAt,
                                       const TxCoordinator::Request&) {
        QMetaObject::invokeMethod(&gui, [&, down, when, releaseAt] {
            {
                std::lock_guard<std::mutex> lk(edgesMu);
                edges.push_back({down, when, releaseAt, Clock::now()});
            }
            CwControlRequest request{CwControlRequest::Kind::Carrier, down, true, 2000, when,   // the over outlives the readout
                                     QStringLiteral("test:iambic"), 0, 0};
            request.releaseAt = releaseAt;
            backend.requestCwControl(request, TxCoordinator::Command{tx.operation, down, {}});
        }, Qt::QueuedConnection);
    });

    // Frames at the wall-clock EP2 rate, on the I/O thread, independent of the GUI thread.
    std::atomic<bool> framesStop{false};
    std::vector<int> iTrace;
    std::thread frames([&] {
        const auto frameDur = std::chrono::nanoseconds(kFrame * 1'000'000'000LL / kRate);
        for (std::int64_t k = 0; !framesStop.load(std::memory_order_acquire); ++k) {
            std::this_thread::sleep_until(t0 + k * frameDur);
            appendI(Hl2TxGateTestAccess::packet(backend), iTrace);
        }
    });

    QTimer stall;
    stall.setTimerType(Qt::PreciseTimer);
    QObject::connect(&stall, &QTimer::timeout, [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(kStallMs));   // the GUI thread is blocked
    });

    keyer.start();
    QEventLoop loop;
    QTimer::singleShot(50, &loop, [&] {
        stall.start(kStallPeriodMs);
        keyer.setPaddleState(true, true);
    });
    QTimer::singleShot(50 + kHoldMs, &loop, [&] { keyer.setPaddleState(false, false); });
    QTimer::singleShot(50 + kHoldMs + 200, &loop, [&] { stall.stop(); });
    QTimer::singleShot(50 + kHoldMs + 600, &loop, &QEventLoop::quit);
    loop.exec();
    keyer.stop();
    QCoreApplication::processEvents();
    framesStop.store(true, std::memory_order_release);
    frames.join();

    result.totals = Hl2TxGateTestAccess::totals(backend);
    std::lock_guard<std::mutex> lk(edgesMu);
    // Precondition, measured on the GUI thread before requestCwControl ran: at
    // least one key-down reached it past the lead plus a frame (so the over
    // had to shift), and none past the shift bound.
    const auto pastLead = std::chrono::nanoseconds((kLeadSamples + kFrame) * 1'000'000'000LL / kRate);
    const auto bound = std::chrono::nanoseconds(
        (kLeadSamples + MetisClientTestAccess::maxShiftSamples() - kFrame) * 1'000'000'000LL / kRate);
    bool shiftNeeded = false;
    std::vector<double> lateMs;
    for (const EdgeSeen& e : edges) {
        const auto late = e.arrival - e.when;
        lateMs.push_back(std::chrono::duration<double, std::milli>(late).count());
        result.maxLateMs = std::max(result.maxLateMs,
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(late).count()));
        if (late > bound) {
            result.why = "an edge reached the GUI thread " + std::to_string(result.maxLateMs)
                       + " ms after its instant, past the shift bound";
            return result;
        }
        shiftNeeded |= e.down && late > pastLead;
    }
    if (!lateMs.empty()) {
        std::sort(lateMs.begin(), lateMs.end());
        result.medianLateMs = lateMs[lateMs.size() / 2];
    }
    if (!shiftNeeded) {
        result.why = "no key-down was held past the lead";
        return result;
    }
    if (edges.size() < 16 || edges.size() % 2 != 0 || !edges.front().down) {
        result.why = "keyer produced " + std::to_string(edges.size()) + " edges";
        return result;
    }
    result.valid = true;

    const auto runs = carriers(iTrace);
    result.elements = edges.size() / 2;
    result.onWire = runs.size();
    for (std::size_t i = 0; i < result.elements; ++i) {
        result.releasePaired &= edges[2 * i].releaseAt == edges[2 * i + 1].when;
    }
    if (runs.size() != result.elements) {
        result.worstLengthErr = std::numeric_limits<std::int64_t>::max();
        return result;
    }
    std::int64_t firstOffset = 0;
    std::int64_t previousOffset = std::numeric_limits<std::int64_t>::min() / 2;   // no underflow at "- 1"
    for (std::size_t i = 0; i < runs.size(); ++i) {
        const std::int64_t down = sampleAt(t0, edges[2 * i].when);
        const std::int64_t up = sampleAt(t0, edges[2 * i + 1].when);
        result.worstLengthErr = std::max(result.worstLengthErr, std::abs(runs[i].length - (up - down)));
        const std::int64_t offset = runs[i].start - (down + kLeadSamples);
        if (i == 0) {
            firstOffset = offset;
        }
        result.placementMonotone &= offset >= previousOffset - 1;
        previousOffset = offset;
        result.offsetGrowth = offset - firstOffset;
        if (i + 1 < runs.size()) {
            const std::int64_t nextDown = sampleAt(t0, edges[2 * i + 2].when);
            const std::int64_t space = runs[i + 1].start - (runs[i].start + runs[i].length);
            const std::int64_t d = space - (nextDown - up);
            result.worstSpaceShort = std::min(result.worstSpaceShort, d);
            result.spaceExcess += d;
            result.stretchedSpaces += d > 1 ? 1 : 0;
        }
    }
    return result;
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-cw-gui-stall"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "Cannot create isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    check(kStallMs > MetisClient::kCwEdgeLeadMs + kFrame * 1000 / kRate,
          "the stall is longer than the lead plus one frame: edges must arrive late");

    for (int attempt = 1; attempt <= kAttempts; ++attempt) {
        const Attempt a = runOnce();
        if (!a.valid) {
            std::printf("attempt %d inconclusive: %s\n", attempt, a.why.c_str());
            continue;
        }
        std::printf("attempt %d: %zu elements from the keyer, %zu on the wire; GUI arrival median %.2f ms, "
                    "latest %d ms; worst element error %lld samples; spaces: %zu stretched, total excess %lld, "
                    "worst short %lld; placement grew %lld samples; I/O shift %lld samples after %llu shifts, "
                    "%llu refused, %llu late ups\n",
                    attempt, a.elements, a.onWire, a.medianLateMs, a.maxLateMs,
                    static_cast<long long>(a.worstLengthErr), a.stretchedSpaces,
                    static_cast<long long>(a.spaceExcess), static_cast<long long>(a.worstSpaceShort),
                    static_cast<long long>(a.offsetGrowth), static_cast<long long>(a.totals.shift),
                    static_cast<unsigned long long>(a.totals.shifted),
                    static_cast<unsigned long long>(a.totals.refused),
                    static_cast<unsigned long long>(a.totals.lateUps));
        const std::string cond = "GUI stalled " + std::to_string(kStallMs) + " ms every "
                               + std::to_string(kStallPeriodMs) + " ms, 30 WPM squeeze";
        check(a.releasePaired, cond + ": every key-down carries its own key-up instant as its release");
        check(a.onWire == a.elements && a.totals.refused == 0, cond + ": every element is on the wire, 0 dropped");
        check(a.worstLengthErr <= 1, cond + ": every element equals the keyer's scheduled length, +-1 sample");
        check(a.worstSpaceShort >= -1, cond + ": no space is shorter than scheduled");
        check(a.stretchedSpaces <= a.totals.shifted
                  && std::abs(a.spaceExcess - a.offsetGrowth) <= static_cast<std::int64_t>(a.elements),
              cond + ": a space is longer only where the over shifted, by the shift's growth");
        check(a.placementMonotone && a.totals.lateUps == 0,
              cond + ": no element placed earlier than the one before it; no late key-up applied");
        std::printf("%d checks, %d failed\n", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }
    std::printf("SKIP: no attempt met the wall-clock precondition; nothing was judged\n");
    return g_failures == 0 ? 77 : 1;
}
