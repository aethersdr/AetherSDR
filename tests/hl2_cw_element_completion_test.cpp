// #7245 review 2: scheduled CW elements through RadioModel, with the model's
// own per-element requests and completions, into Hl2Backend and its EP2
// writer. Socket-free: frames come from buildNextControlPacket() on the
// backend's I/O thread, built by the test, so the frame cursor and not the
// wall clock decides when an edge is due. Every edge has reached the I/O
// thread, and every completion the model, before the frames it falls in.
//
// Pinned: an element's key-up completion (which ends its intent) is released
// only once its scheduled release is on EP2, under manual MOX and break-in;
// a paddle's prequeued release leaves a separately held straight key keyed;
// a straight key released during a paddle element ends with that element;
// a revoked producer's element is not keyed, or stops, at the EP2 writer.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/TxCoordinator.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2Settings.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QString>
#include <QVariantMap>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
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
    static std::size_t pending(const MetisClient& client) { return client.m_cwScheduleCount; }
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
    static bool keyed(const Hl2Backend& backend) { return backend.m_keyed; }
    static std::size_t pending(Hl2Backend& backend)
    {
        std::size_t n = 0;
        QMetaObject::invokeMethod(backend.m_metis, [&n, metis = backend.m_metis] {
            n = MetisClientTestAccess::pending(*metis);
        }, Qt::BlockingQueuedConnection);
        return n;
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
static std::int64_t samplesOfMs(std::int64_t ms) { return ms * kRate / 1000; }
static bool within(std::int64_t got, std::int64_t want, std::int64_t tol) { return got >= want - tol && got <= want + tol; }

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

// A RadioModel with an Hl2Backend, as testRefusedOverRaisesOneNotice in
// hl2_cw_edge_timing_test builds it. Paddle elements go through
// queueProducerCwKeyEdge (the iambic keyer's path, one derived request per
// element), a straight key through requestProducerCw, as MainWindow does.
struct Rig {
    RadioModel model;
    Hl2Backend* backend = nullptr;
    Clock::time_point t0;
    std::vector<int> iTrace;
    int refusedDowns = 0;   // paddle key-downs the model did not forward
    explicit Rig(bool breakIn, int leadMs = Hl2Settings::kCwEdgeLeadDefaultMs)
    {
        model.setBackendForTest(std::make_unique<Hl2Backend>(), QStringLiteral("hl2"));
        backend = static_cast<Hl2Backend*>(model.backend());
        if (leadMs != Hl2Settings::kCwEdgeLeadDefaultMs) {
            backend->invokeExtension(QStringLiteral("hl2"), QStringLiteral("cwlead.set"), 0,
                                     QVariantMap{{QStringLiteral("edgeLeadMs"), leadMs}});
        }
        t0 = Clock::now();
        Hl2TxGateTestAccess::prepare(*backend, t0);
        model.transmitModel().setCwDelay(1000);   // the hang outlives every readout here
        model.transmitModel().setCwBreakIn(breakIn);
        settle();
    }
    ~Rig() { AppSettings::instance().remove(QStringLiteral("Hl2")); }
    // Queued hops: model, backend, I/O thread, and completions back to the model.
    static void settle()
    {
        QElapsedTimer waited;
        waited.start();
        while (waited.elapsed() < 100) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
    }
    Clock::time_point at(std::int64_t ms) const { return t0 + std::chrono::milliseconds(ms); }
    void element(const TxCoordinator::Request& element, std::int64_t downMs, std::int64_t upMs)
    {
        model.queueProducerCwKeyEdge(element, true, QStringLiteral("test:paddle"), 0, 0, at(downMs),
                                     false, [this] { ++refusedDowns; }, at(upMs));
        model.queueProducerCwKeyEdge(element, false, QStringLiteral("test:paddle"), 0, 0, at(upMs));
    }
    void buildTo(std::int64_t ms)
    {
        while (static_cast<std::int64_t>(iTrace.size()) < samplesOfMs(ms)) {
            appendI(Hl2TxGateTestAccess::packet(*backend), iTrace);
        }
    }
};

static std::string runsText(const std::vector<Run>& runs)
{
    std::string text = std::to_string(runs.size()) + " runs";
    for (const Run& r : runs) {
        text += " [" + std::to_string(r.start * 1000 / kRate) + " ms, " + std::to_string(r.length) + " samples]";
    }
    return text;
}

// Blocker 1, manual MOX: the dit's key-up reaches the I/O thread 20 ms before
// its key-down is due (40 ms lead). Its completion must not end the
// element's intent before the element is on the wire.
static void testManualMoxDitSurvivesItsKeyUp()
{
    Rig rig(false);
    rig.model.transmitModel().setMox(true);
    Rig::settle();
    check(Hl2TxGateTestAccess::keyed(*rig.backend), "manual MOX: the backend is keyed");
    const TxCoordinator::Producer paddle = rig.model.registerTxProducer();
    rig.element(paddle.request().derive(), 100, 120);
    Rig::settle();
    check(rig.model.txProducerHasWork(paddle),
          "manual MOX: the element's work is open while its release is still pending at EP2");
    rig.buildTo(400);
    const auto runs = carriers(rig.iTrace);
    check(runs.size() == 1 && within(runs[0].length, samplesOfMs(20), 1),
          "manual MOX, 40 ms lead: a 20 ms dit whose key-up arrived first is 20 ms on EP2 (" + runsText(runs) + ")");
    Rig::settle();
    check(!rig.model.txProducerHasWork(paddle),
          "manual MOX: the element's work ends once its release is on EP2");
    rig.model.transmitModel().setMox(false);
    Rig::settle();
}

// Blocker 1, break-in at the 100 ms lead: the second dit's key-down used to
// drop the first dit's hang completion, ending the first intent before its
// element was due.
static void testBreakInTwoDitsBothReachEp2()
{
    Rig rig(true, Hl2Settings::kCwEdgeLeadMaxMs);
    const TxCoordinator::Producer paddle = rig.model.registerTxProducer();
    const TxCoordinator::Request input = paddle.request();
    rig.element(input.derive(), 100, 120);
    rig.element(input.derive(), 140, 160);
    Rig::settle();
    check(Hl2TxGateTestAccess::keyed(*rig.backend), "break-in: the first element keyed the backend");
    rig.buildTo(600);
    const auto runs = carriers(rig.iTrace);
    check(runs.size() == 2 && within(runs[0].length, samplesOfMs(20), 1) && within(runs[1].length, samplesOfMs(20), 1)
              && within(runs[1].start - runs[0].start, samplesOfMs(40), 1),
          "break-in, 100 ms lead: both admitted 20 ms dits are on EP2, 40 ms apart (" + runsText(runs) + ")");
}

// Blocker 2: a straight key held through a paddle element. The model
// suppresses the paddle's key-up; the release prequeued with the paddle's
// key-down must not key the straight key up.
static void testPaddleReleaseLeavesAStraightKeyHeld()
{
    Rig rig(true);
    const TxCoordinator::Request straight = rig.model.registerTxProducer().request();
    check(rig.model.requestProducerCw(straight, true), "hold: the straight key-down is admitted");
    Rig::settle();
    rig.buildTo(50);
    const TxCoordinator::Producer paddle = rig.model.registerTxProducer();
    const TxCoordinator::Request element = paddle.request().derive();
    // The down reaches the I/O thread before the suppressed up ends its intent.
    rig.model.queueProducerCwKeyEdge(element, true, QStringLiteral("test:paddle"), 0, 0, rig.at(100), false,
                                     [&rig] { ++rig.refusedDowns; }, rig.at(140));
    Rig::settle();
    const std::size_t pendingEdges = Hl2TxGateTestAccess::pending(*rig.backend);
    check(pendingEdges == 2 && rig.refusedDowns == 0,
          "hold: the paddle element is scheduled at EP2 (" + std::to_string(pendingEdges) + " pending, "
              + std::to_string(rig.refusedDowns) + " refused)");
    rig.model.queueProducerCwKeyEdge(element, false, QStringLiteral("test:paddle"), 0, 0, rig.at(140));
    Rig::settle();
    rig.buildTo(300);
    auto runs = carriers(rig.iTrace);
    check(runs.size() == 1 && runs[0].start < samplesOfMs(50) + kFrame
              && runs[0].start + runs[0].length == static_cast<std::int64_t>(rig.iTrace.size()),
          "hold: the straight key stays keyed on EP2 through the paddle's release at 180 ms (" + runsText(runs) + ")");
    (void)rig.model.requestProducerCw(straight, false);
    Rig::settle();
    rig.buildTo(400);
    runs = carriers(rig.iTrace);
    check(runs.size() == 1 && within(runs[0].start + runs[0].length, samplesOfMs(300), 2 * kFrame),
          "hold: the straight key's own release keys up (" + runsText(runs) + ")");
}

// Ordinary release: the straight key goes up during the paddle element (the
// model suppresses that up and ends its intent), then the paddle's key-up is
// forwarded. The carrier ends with the paddle's release, not later.
static void testStraightKeyReleasedDuringAPaddleElement()
{
    Rig rig(true);
    const TxCoordinator::Request straight = rig.model.registerTxProducer().request();
    check(rig.model.requestProducerCw(straight, true), "overlap: the straight key-down is admitted");
    Rig::settle();
    rig.buildTo(50);
    const TxCoordinator::Producer paddle = rig.model.registerTxProducer();
    const TxCoordinator::Request element = paddle.request().derive();
    rig.model.queueProducerCwKeyEdge(element, true, QStringLiteral("test:paddle"), 0, 0, rig.at(100), false, {},
                                     rig.at(140));
    Rig::settle();
    (void)rig.model.requestProducerCw(straight, false);
    rig.model.queueProducerCwKeyEdge(element, false, QStringLiteral("test:paddle"), 0, 0, rig.at(140));
    Rig::settle();
    rig.buildTo(400);
    const auto runs = carriers(rig.iTrace);
    // The run ends at the half point of the 5 ms fall that starts at the release sample.
    const std::int64_t release = samplesOfMs(140) + samplesOfMs(Hl2Settings::kCwEdgeLeadDefaultMs);
    check(runs.size() == 1 && runs[0].start < samplesOfMs(50) + kFrame
              && within(runs[0].start + runs[0].length, release + samplesOfMs(5) / 2, 1),
          "overlap: one carrier, ending at the paddle's release sample (" + runsText(runs) + ")");
}

// Producer revocation at the EP2 writer: before the element is due, nothing
// is keyed; during it, the carrier stops within a frame.
static void testRevokedProducerAtTheWriter()
{
    {
        Rig rig(true);
        const TxCoordinator::Producer paddle = rig.model.registerTxProducer();
        rig.element(paddle.request().derive(), 100, 140);
        Rig::settle();
        paddle.invalidate();
        rig.buildTo(400);
        const auto runs = carriers(rig.iTrace);
        check(runs.empty(), "revoked before its element: nothing on EP2 (" + runsText(runs) + ")");
    }
    {
        Rig rig(true);
        const TxCoordinator::Producer paddle = rig.model.registerTxProducer();
        rig.element(paddle.request().derive(), 100, 400);
        Rig::settle();
        rig.buildTo(200);
        paddle.invalidate();
        rig.buildTo(600);
        const auto runs = carriers(rig.iTrace);
        check(runs.size() == 1 && within(runs[0].start, samplesOfMs(140), samplesOfMs(5))
                  && runs[0].start + runs[0].length <= samplesOfMs(200) + 2 * kFrame,
              "revoked during its element: the carrier stops at the writer (" + runsText(runs) + ")");
    }
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-cw-element-completion"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "Cannot create isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    testManualMoxDitSurvivesItsKeyUp();
    testBreakInTwoDitsBothReachEp2();
    testPaddleReleaseLeavesAStraightKeyHeld();
    testStraightKeyReleasedDuringAPaddleElement();
    testRevokedProducerAtTheWriter();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
