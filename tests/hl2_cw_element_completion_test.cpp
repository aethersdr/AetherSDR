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
// a straight key's suppressed up after the last release ends its hold;
// a revoked producer's element is not keyed, or stops, at the EP2 writer;
// a host-text element's fall is whole when the next down is already queued;
// HostCwTextEngine's natural end leaves its last element whole on EP2, and
// the same sender's next text a word space after it (the wall-clock cases:
// its real keyer times the text, frames follow 5 ms behind); a host-text run
// fenced inside the break-in hang leaves the next text keyed on EP2 (#7567);
// an element after such a fence, due in the next EP2 frame, is whole even
// when the pacer builds frames right after its key-down reaches the I/O
// thread, because admission and the MOX re-raise are one action (#7572); the
// same holds for the two other fences of a host-text run inside the hang, a
// refused key-down and the retire cap; a re-key after such a fence that the
// EP2 writer refuses (past the shift bound) raises no MOX; and a re-key in a
// live over leaves its MOX alone; under manual MOX the last paddle element's
// fall is whole when the paddles go idle with MOX held (#7568).

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/TxCoordinator.h"
#include "core/backends/SliceDelta.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2Settings.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"
#include "models/HostCwTextEngine.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QString>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
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
    static TxCoordinator::Operation moxOperation(const MetisClient& client) { return client.m_moxOperation; }
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
    static MetisClient* metis(Hl2Backend& backend) { return backend.m_metis; }
    // MOX as the EP2 writer holds it, not as the backend believes it.
    static bool wireKeyed(Hl2Backend& backend)
    {
        bool keyed = false;
        QMetaObject::invokeMethod(backend.m_metis, [&keyed, metis = backend.m_metis] {
            keyed = metis->isKeyed();
        }, Qt::BlockingQueuedConnection);
        return keyed;
    }
    // The EP2 writer's fail-safe counters: shifted downs, late ups, refused downs.
    static std::string counters(Hl2Backend& backend)
    {
        std::string text;
        QMetaObject::invokeMethod(backend.m_metis, [&text, metis = backend.m_metis] {
            text = "shifted " + std::to_string(metis->cwShiftedElements()) + ", late ups "
                + std::to_string(metis->cwLateKeyUps()) + ", refused " + std::to_string(metis->cwRefusedDowns());
        }, Qt::BlockingQueuedConnection);
        return text;
    }
    // The operation the EP2 writer holds MOX under.
    static TxCoordinator::Operation moxOperation(Hl2Backend& backend)
    {
        TxCoordinator::Operation operation;
        QMetaObject::invokeMethod(backend.m_metis, [&operation, metis = backend.m_metis] {
            operation = MetisClientTestAccess::moxOperation(*metis);
        }, Qt::BlockingQueuedConnection);
        return operation;
    }
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

namespace AetherSDR {
// The retire cap: every finished run's producer is polled as if kRetireWaitMs
// had passed. The hang outlasts no cap offline (2000 ms + lead + shift bound
// is under 2500 ms), so a test reaches it only this way.
struct HostCwTextEngineTestAccess {
    static bool inputPending(const HostCwTextEngine& engine)
    {
        return engine.m_model && std::any_of(engine.m_retiring.cbegin(), engine.m_retiring.cend(),
            [&engine](const HostCwTextEngine::Retiring& r) { return engine.m_model->txProducerInputPending(r.producer); });
    }
    static void expireRetireWait(HostCwTextEngine& engine)
    {
        std::vector<int> ids;
        for (const HostCwTextEngine::Retiring& r : engine.m_retiring) {
            ids.push_back(r.id);
        }
        for (const int id : ids) {
            engine.pollRetiring(id, HostCwTextEngine::kRetireWaitMs);
        }
    }
};
}  // namespace AetherSDR

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

// The EP2 pacer, run on the I/O thread the moment a queued call there has
// admitted a key-down (the schedule grew), before the next queued call is
// delivered. Models the worker reaching onEp2PacerTick() between two calls the
// model thread posted back to back. Lives on the I/O thread; read it there.
class PacerAfterAdmittedDown : public QObject {
public:
    PacerAfterAdmittedDown(MetisClient* metis, int frames) : m_metis(metis), m_frames(frames) {}
    std::vector<int> trace;
    bool fired = false;
protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (fired || watched != m_metis || event->type() != QEvent::MetaCall) {
            return false;
        }
        const std::size_t before = MetisClientTestAccess::pending(*m_metis);
        watched->event(event);   // deliver this call now...
        if (MetisClientTestAccess::pending(*m_metis) > before) {
            fired = true;        // ...and run the pacer before the next one
            for (int k = 0; k < m_frames; ++k) {
                appendI(m_metis->buildNextControlPacket(), trace);
            }
        }
        return true;
    }
private:
    MetisClient* m_metis;
    int m_frames;
};

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

// The per-frame hold prune: a straight key's up that the model suppresses
// (a paddle element is still down in the model) after that element's
// scheduled release is already on EP2. Nothing is scheduled or keyed then,
// so no release prunes the straight key's lapsed hold; the frame must, or
// the carrier stays up on a hold whose intent has ended.
static void testSuppressedStraightUpAfterTheLastRelease()
{
    Rig rig(true);
    const TxCoordinator::Request straight = rig.model.registerTxProducer().request();
    check(rig.model.requestProducerCw(straight, true), "prune: the straight key-down is admitted");
    Rig::settle();
    rig.buildTo(50);
    const TxCoordinator::Producer paddle = rig.model.registerTxProducer();
    const TxCoordinator::Request element = paddle.request().derive();
    rig.model.queueProducerCwKeyEdge(element, true, QStringLiteral("test:paddle"), 0, 0, rig.at(100), false, {},
                                     rig.at(140));
    Rig::settle();
    rig.buildTo(250);   // the paddle's release (180 ms) is written; nothing is pending
    (void)rig.model.requestProducerCw(straight, false);
    Rig::settle();
    rig.buildTo(400);
    const auto runs = carriers(rig.iTrace);
    check(runs.size() == 1 && runs[0].start + runs[0].length <= samplesOfMs(250) + 2 * kFrame,
          "prune: the straight key's lapsed hold ends the carrier with no release pending (" + runsText(runs) + ")");
    rig.model.queueProducerCwKeyEdge(element, false, QStringLiteral("test:paddle"), 0, 0, rig.at(140));
    Rig::settle();
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

// Review 3, blocker 1: host-text CW (no releaseAt; a separate scheduled
// key-up per element) in break-in, with a space shorter than the lead. The
// next element's key-down reaches the I/O thread before this element's
// key-up is written. Writing that key-up must not hand the CW operation back
// to this element, whose completion then ends its intent inside the 5 ms
// fall: the frame after it would step the envelope to zero, a key click.
static void testHostTextFallSurvivesTheNextDown()
{
    Rig rig(true);
    const TxCoordinator::Producer text = rig.model.registerTxProducer();
    const TxCoordinator::Request input = text.request();
    // 40 WPM, 30 ms units, as HostCwTextKeyer queues them: a down, then its own
    // up, no releaseAt. Its hostTextGates are admission only; this rig has no
    // TX slice to pass them, so they are left off.
    const auto edge = [&rig](const TxCoordinator::Request& element, bool down, std::int64_t ms) {
        rig.model.queueProducerCwKeyEdge(element, down, QStringLiteral("test:text"), 0, 0, rig.at(ms),
                                         /*hostTextGates=*/false,
                                         down ? std::function<void()>{[&rig] { ++rig.refusedDowns; }}
                                              : std::function<void()>{});
    };
    const TxCoordinator::Request first = input.derive();
    edge(first, true, 100);
    edge(first, false, 130);
    const TxCoordinator::Request second = input.derive();
    edge(second, true, 160);   // queued before the first up (due at 170 ms) is written
    edge(second, false, 190);
    Rig::settle();
    check(rig.refusedDowns == 0 && Hl2TxGateTestAccess::pending(*rig.backend) == 4,
          "host text: both elements are scheduled at EP2 (" + std::to_string(rig.refusedDowns) + " refused)");
    const std::int64_t up = samplesOfMs(130) + samplesOfMs(Hl2Settings::kCwEdgeLeadDefaultMs);
    // Through the frame that writes the first up, then let its completion
    // reach the model inside the fall.
    while (static_cast<std::int64_t>(rig.iTrace.size()) <= up) {
        appendI(Hl2TxGateTestAccess::packet(*rig.backend), rig.iTrace);
    }
    Rig::settle();
    rig.buildTo(400);
    const std::int64_t fall = samplesOfMs(5);
    bool monotonic = true;
    int largestStep = 0;
    for (std::int64_t n = up + 1; n <= up + fall; ++n) {
        const int step = rig.iTrace[static_cast<std::size_t>(n - 1)] - rig.iTrace[static_cast<std::size_t>(n)];
        monotonic = monotonic && step >= 0;
        largestStep = std::max(largestStep, step);
    }
    // A raised-cosine fall over 240 samples drops at most ~215 per sample.
    std::string trace;
    for (std::int64_t n = up; n <= up + fall; n += 20) {
        trace += " " + std::to_string(rig.iTrace[static_cast<std::size_t>(n)]);
    }
    check(monotonic && largestStep < 400 && rig.iTrace[static_cast<std::size_t>(up + fall - 20)] > 0
              && rig.iTrace[static_cast<std::size_t>(up + fall)] == 0,
          "host text, break-in, 40 ms lead: the first element's 5 ms fall decreases monotonically to 0 (I every 20 samples:"
              + trace + "; largest step " + std::to_string(largestStep) + ")");
    const auto runs = carriers(rig.iTrace);
    check(runs.size() == 2 && within(runs[1].start - runs[0].start, samplesOfMs(60), 1),
          "host text: both elements are on EP2, 60 ms apart (" + runsText(runs) + ")");
}

// Review 4, blocker 1: the real HostCwTextEngine and its CwxLocalKeyer play
// "EE". The keyer drains at the last key-up's instant, a lead before that
// key-up is on EP2; the run must not end, and fence its producer, until it is.
// Frames are built 5 ms behind the wall clock, as the I/O thread builds them.
static void testHostTextEngineLastElementIsWhole(int wpm, bool breakIn)
{
    const std::string name = "host text engine, " + std::to_string(wpm) + " WPM, "
        + (breakIn ? "break-in" : "manual MOX") + ": ";
    Rig rig(breakIn);
    SliceDelta slice;   // the host-text gates key only with a CW TX slice
    slice.frequency = 7.020;
    slice.mode = QStringLiteral("CW");
    slice.panId = QStringLiteral("0x40000000");
    slice.txSlice = true;
    rig.model.emitBackendSliceChangedForTest(0, slice);
    if (!breakIn) {
        rig.model.transmitModel().setMox(true);
        Rig::settle();
    }
    const TxCoordinator::Producer client = rig.model.registerTxProducer();
    bool ended = false;
    bool refused = false;
    bool playingAtEnd = true;
    std::size_t builtAtFence = 0;
    std::unique_ptr<HostCwTextEngine> engine;
    engine = std::make_unique<HostCwTextEngine>(&rig.model, [&](const TxCoordinator::Request&, bool wasRefused) {
        ended = true;
        refused = wasRefused;
        playingAtEnd = engine->playing();
    });
    check(engine->play(client.request(), QStringLiteral("EE"), wpm, {}), name + "the engine plays");
    QElapsedTimer wall;
    wall.start();
    std::int64_t fencedMs = -1;
    while (wall.elapsed() < 4000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        const std::int64_t ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - rig.t0).count() - 5;
        rig.buildTo(ms);
        if (ended && !engine->retiring() && fencedMs < 0) {
            fencedMs = ms;
            builtAtFence = rig.iTrace.size();
        }
        if (fencedMs >= 0 && ms > fencedMs + 200) {
            break;
        }
    }
    const std::int64_t unit = 1200 / wpm;
    const auto runs = carriers(rig.iTrace);
    check(runs.size() == 2 && within(runs[0].length, samplesOfMs(unit), 2)
              && within(runs[1].length, samplesOfMs(unit), 2),
          name + "both dits are whole on EP2 (" + runsText(runs) + "; "
              + Hl2TxGateTestAccess::counters(*rig.backend) + ")");
    const std::int64_t lastEnd = runs.empty() ? 0 : runs.back().start + runs.back().length;
    check(ended && !refused && !playingAtEnd,
          name + "the run ends, unrefused, and is no longer playing when it does");
    check(fencedMs >= 0 && static_cast<std::int64_t>(builtAtFence) >= lastEnd,
          name + "its producer is fenced after its last element is on EP2 (fenced at sample "
              + std::to_string(builtAtFence) + ", last carrier ends at " + std::to_string(lastEnd) + ")");
    if (!breakIn) {
        rig.model.transmitModel().setMox(false);
        Rig::settle();
    }
}

// Review 5, blocker 1: the same sender's next text, played while the first
// run's last element is still on its way to EP2 (or, `intoDrainedKeyer`, while
// the drain's notice has not reached the model, so the run still plays), is
// keyed a word space (7 units) after the first run's last key-up.
static void testHostTextSameSenderNextTextIsSpaced(std::int64_t secondAtMs, bool breakIn, bool intoDrainedKeyer)
{
    const std::string name = "host text, second \"E\" " + std::to_string(secondAtMs) + " ms after the first, "
        + (breakIn ? "break-in" : "manual MOX") + (intoDrainedKeyer ? ", into the drained keyer" : "") + ": ";
    Rig rig(breakIn, intoDrainedKeyer ? Hl2Settings::kCwEdgeLeadMaxMs : Hl2Settings::kCwEdgeLeadDefaultMs);
    SliceDelta slice;
    slice.frequency = 7.020;
    slice.mode = QStringLiteral("CW");
    slice.panId = QStringLiteral("0x40000000");
    slice.txSlice = true;
    rig.model.emitBackendSliceChangedForTest(0, slice);
    if (!breakIn) {
        rig.model.transmitModel().setMox(true);
        Rig::settle();
    }
    const TxCoordinator::Producer client = rig.model.registerTxProducer();
    const TxCoordinator::Request sender = client.request();
    int endedRuns = 0;
    HostCwTextEngine engine(&rig.model, [&](const TxCoordinator::Request&, bool) { ++endedRuns; });
    constexpr int kWpm = 20;
    constexpr std::int64_t kUnit = 1200 / kWpm;
    const auto wallMs = [&rig] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - rig.t0).count();
    };
    const std::int64_t startMs = wallMs();
    check(engine.play(sender, QStringLiteral("E"), kWpm, {}), name + "the first message plays");
    bool second = false;
    bool playingAtSecond = false;
    QElapsedTimer wall;
    wall.start();
    while (wall.elapsed() < 4000 && !(second && endedRuns >= 2 && !engine.retiring())) {
        if (!second && wallMs() - startMs >= secondAtMs) {
            playingAtSecond = engine.playing();
            check(engine.play(sender, QStringLiteral("E"), kWpm, {}), name + "the second message plays");
            second = true;
        }
        if (intoDrainedKeyer && !second && wallMs() - startMs >= 40) {
            // Hold the model thread across the drain (60 ms): its notice waits,
            // so the second message finds the run playing and its keyer drained.
            while (wallMs() - startMs < secondAtMs) {
            }
            continue;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        rig.buildTo(wallMs() - 5);
    }
    rig.buildTo(wallMs() - 5 + 200);
    if (intoDrainedKeyer) {
        check(playingAtSecond, name + "the run still plays when the second message arrives");
    }
    const auto runs = carriers(rig.iTrace);
    const std::int64_t gap = runs.size() == 2 ? runs[1].start - (runs[0].start + runs[0].length) : -1;
    check(runs.size() == 2 && within(runs[0].length, samplesOfMs(kUnit), 2) && within(runs[1].length, samplesOfMs(kUnit), 2)
              && gap >= samplesOfMs(7 * kUnit - 2) && gap <= samplesOfMs(7 * kUnit + 20),
          name + "both dits whole, a word space (420 ms) apart on EP2 (" + runsText(runs) + ", gap "
              + std::to_string(gap * 1000 / kRate) + " ms)");
    if (!breakIn) {
        rig.model.transmitModel().setMox(false);
        Rig::settle();
    }
}

// #7567: a host-text run fenced inside the break-in hang (a CWX clear,
// stopAll()) drops MOX at the writer while the backend stays keyed. The next
// text inside the same hang must still put RF on EP2, or be refused; it must
// never be admitted and keyed silently. Manual MOX is the control: the
// operator's own MOX survives the fence.
static void testHostTextFencedInTheHangNextTextIsOnTheWire(bool breakIn)
{
    const std::string name = std::string("host text fenced in the hang, ")
        + (breakIn ? "break-in" : "manual MOX") + ": ";
    Rig rig(breakIn);
    SliceDelta slice;
    slice.frequency = 7.020;
    slice.mode = QStringLiteral("CW");
    slice.panId = QStringLiteral("0x40000000");
    slice.txSlice = true;
    rig.model.emitBackendSliceChangedForTest(0, slice);
    if (!breakIn) {
        rig.model.transmitModel().setMox(true);
        Rig::settle();
    }
    const TxCoordinator::Producer client = rig.model.registerTxProducer();
    const TxCoordinator::Request sender = client.request();
    int endedRuns = 0;
    bool secondRefused = false;
    HostCwTextEngine engine(&rig.model, [&](const TxCoordinator::Request&, bool refused) {
        ++endedRuns;
        secondRefused = secondRefused || (endedRuns >= 2 && refused);
    });
    constexpr int kWpm = 20;
    const auto wallMs = [&rig] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - rig.t0).count();
    };
    const auto pump = [&](std::int64_t untilMs) {
        while (wallMs() < untilMs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            rig.buildTo(wallMs() - 5);
        }
    };
    const std::int64_t startMs = wallMs();
    check(engine.play(sender, QStringLiteral("E"), kWpm, {}), name + "the first message plays");
    // The dit (60 ms) is on EP2 a lead after it; the 1000 ms hang runs on.
    pump(startMs + 250);
    const bool keyedAtFence = Hl2TxGateTestAccess::keyed(*rig.backend);
    engine.stopAll();   // the CWX clear: fences the run's producer now
    pump(startMs + 350);
    const bool keyedAtSecond = Hl2TxGateTestAccess::keyed(*rig.backend);
    check(keyedAtFence && keyedAtSecond, name + "the fence and the second text both fall inside the hang");
    check(engine.play(sender, QStringLiteral("E"), kWpm, {}), name + "the second message plays");
    pump(startMs + 350 + 800);
    const auto runs = carriers(rig.iTrace);
    check(secondRefused || (runs.size() == 2 && within(runs[1].length, samplesOfMs(1200 / kWpm), 2)),
          name + "the second dit is whole on EP2, or refused (" + runsText(runs) + "; "
              + Hl2TxGateTestAccess::counters(*rig.backend) + ")");
    if (!breakIn) {
        rig.model.transmitModel().setMox(false);
        Rig::settle();
    }
}

// After a fence in the break-in hang, the next element's key-down is due in
// the very next EP2 frame, and the pacer builds frames as soon as that
// key-down is admitted on the I/O thread. Any frame built between admitting
// the down and raising MOX would pass the down's sample unkeyed and cut the
// element short, with no shift and no refusal, so the element must be whole
// at its sample (#7572).
static void testRekeyAfterAFenceIsOneIoAction()
{
    const std::string name = "re-key after a fence, down due in the next frame, pacer between the calls: ";
    Rig rig(true);
    const TxCoordinator::Producer first = rig.model.registerTxProducer();
    rig.element(first.request().derive(), 100, 160);
    Rig::settle();
    rig.buildTo(300);   // the first dit (140-200 ms on EP2) is written; the hang runs on
    first.invalidate(); // the fence: the operation holding MOX lapses
    rig.buildTo(320);
    check(Hl2TxGateTestAccess::keyed(*rig.backend) && !Hl2TxGateTestAccess::wireKeyed(*rig.backend),
          name + "the fence dropped MOX at the writer inside the hang");
    // The next down lands in the frame after the cursor.
    const std::int64_t cursor = static_cast<std::int64_t>(rig.iTrace.size());
    const std::int64_t perMs = kRate / 1000;
    const std::int64_t downMs = (cursor + perMs - 1) / perMs - Hl2Settings::kCwEdgeLeadDefaultMs;
    const std::int64_t target = (downMs + Hl2Settings::kCwEdgeLeadDefaultMs) * perMs;
    check(target >= cursor && target < cursor + kFrame,
          name + "the down is due in the next frame (cursor " + std::to_string(cursor) + ", down at "
              + std::to_string(target) + ")");
    auto* pacer = new PacerAfterAdmittedDown(Hl2TxGateTestAccess::metis(*rig.backend),
                                             static_cast<int>(samplesOfMs(10) / kFrame) + 1);
    MetisClient* metis = Hl2TxGateTestAccess::metis(*rig.backend);
    pacer->moveToThread(metis->thread());
    QMetaObject::invokeMethod(metis, [metis, pacer] { metis->installEventFilter(pacer); },
                              Qt::BlockingQueuedConnection);
    const TxCoordinator::Producer second = rig.model.registerTxProducer();
    rig.element(second.request().derive(), downMs, downMs + 60);
    Rig::settle();
    bool fired = false;
    QMetaObject::invokeMethod(metis, [&] {
        metis->removeEventFilter(pacer);
        fired = pacer->fired;
        rig.iTrace.insert(rig.iTrace.end(), pacer->trace.begin(), pacer->trace.end());
        delete pacer;
    }, Qt::BlockingQueuedConnection);
    check(fired && rig.refusedDowns == 0, name + "the pacer ran right after the down was admitted");
    rig.buildTo(800);
    const auto runs = carriers(rig.iTrace);
    check(runs.size() == 2 && within(runs[1].length, samplesOfMs(60), 2)
              && within(runs[1].start, target + samplesOfMs(5) / 2, 2),
          name + "the element is whole on EP2, at its sample (" + runsText(runs) + "; "
              + Hl2TxGateTestAccess::counters(*rig.backend) + ")");
}

// The re-key's MOX is raised only for a down the EP2 writer
// admitted. Here the down after the fence is later than the shift bound, so
// setCwKeyDown() refuses it; MOX must stay down at the writer for the rest of
// the hang, or the radio keys with no carrier until the hang ends.
static void testRefusedRekeyAfterAFenceRaisesNoMox()
{
    const std::string name = "re-key after a fence, refused past the shift bound: ";
    Rig rig(true);
    const TxCoordinator::Producer first = rig.model.registerTxProducer();
    rig.element(first.request().derive(), 100, 160);
    Rig::settle();
    rig.buildTo(300);   // the first dit (140-200 ms on EP2) is written; the hang runs on
    first.invalidate(); // the fence: the operation holding MOX lapses
    rig.buildTo(600);   // the frame cursor runs ahead of the next element
    check(Hl2TxGateTestAccess::keyed(*rig.backend) && !Hl2TxGateTestAccess::wireKeyed(*rig.backend),
          name + "the fence dropped MOX at the writer inside the hang");
    // After the first element for the model, but due on air at 240 ms against
    // a 600 ms cursor: 360 ms late, past the 250 ms shift bound.
    const TxCoordinator::Producer second = rig.model.registerTxProducer();
    rig.element(second.request().derive(), 200, 260);
    Rig::settle();
    const std::string counters = Hl2TxGateTestAccess::counters(*rig.backend);
    check(counters.find("refused 1") != std::string::npos && rig.refusedDowns == 0,
          name + "the down reached the EP2 writer and was refused there (" + counters + "; " + std::to_string(rig.refusedDowns) + " refused by the model)");
    check(Hl2TxGateTestAccess::keyed(*rig.backend) && !Hl2TxGateTestAccess::wireKeyed(*rig.backend),
          name + "the refused down raised no MOX at the writer");
    rig.buildTo(800);
    const auto runs = carriers(rig.iTrace);
    check(!Hl2TxGateTestAccess::wireKeyed(*rig.backend) && runs.size() == 1,
          name + "no MOX and no carrier after it (" + runsText(runs) + ")");
}

// Inside the hang with no fence, the over is live and its
// MOX is left alone. The re-key admits the down and calls nothing else: the
// writer keeps MOX under the first element's operation, not the new one's.
static void testRekeyInALiveOverLeavesItsMoxAlone()
{
    const std::string name = "re-key in a live over: ";
    Rig rig(true);
    const TxCoordinator::Producer first = rig.model.registerTxProducer();
    rig.element(first.request().derive(), 100, 160);
    Rig::settle();
    rig.buildTo(300);
    const TxCoordinator::Operation held = Hl2TxGateTestAccess::moxOperation(*rig.backend);
    check(Hl2TxGateTestAccess::keyed(*rig.backend) && Hl2TxGateTestAccess::wireKeyed(*rig.backend),
          name + "the hang holds MOX at the writer");
    const TxCoordinator::Producer second = rig.model.registerTxProducer();
    rig.element(second.request().derive(), 400, 460);
    Rig::settle();
    const TxCoordinator::Operation after = Hl2TxGateTestAccess::moxOperation(*rig.backend);
    check(rig.refusedDowns == 0 && after.sameAuthority(held),
          name + "MOX is still held under the first element's operation");
    rig.buildTo(800);
    const auto runs = carriers(rig.iTrace);
    check(runs.size() == 2 && within(runs[1].length, samplesOfMs(60), 2),
          name + "the second element is whole on EP2 (" + runsText(runs) + ")");
}

// The two other routes by which a host-text run's
// producer is fenced inside the break-in hang, into the same #7567 fix.
// Refusal: the run's second key-down is refused at the host-text gates (the
// TX slice leaves CW), and refused() fences the run at once. Retire cap: the
// drained run's input is still pending in the hang when kRetireWaitMs expires,
// and the cap fences it anyway. Either way the next text inside the same hang
// is whole on EP2, or refused with a notice; never admitted silently. Manual
// MOX is the control: the operator's MOX survives the fence.
enum class HangFence { Refusal, RetireCap };
static void testHostTextOtherFenceInTheHang(HangFence route, bool breakIn)
{
    const bool refusal = route == HangFence::Refusal;
    const std::string name = std::string("host text fenced in the hang by ")
        + (refusal ? "a refused key-down" : "the retire cap") + ", " + (breakIn ? "break-in" : "manual MOX") + ": ";
    Rig rig(breakIn);
    const auto setMode = [&rig](const QString& mode) {
        SliceDelta slice;
        slice.frequency = 7.020;
        slice.mode = mode;
        slice.panId = QStringLiteral("0x40000000");
        slice.txSlice = true;
        rig.model.emitBackendSliceChangedForTest(0, slice);
    };
    setMode(QStringLiteral("CW"));
    if (!breakIn) {
        rig.model.transmitModel().setMox(true);
        Rig::settle();
    }
    int notices = 0;
    QObject::connect(&rig.model, &RadioModel::interlockNotificationRequested,
                     [&notices](const QString&, const QString& key, const QString&) {
                         notices += key.startsWith(QLatin1String("cw-text-")) ? 1 : 0;
                     });
    const TxCoordinator::Producer client = rig.model.registerTxProducer();
    const TxCoordinator::Request sender = client.request();
    int endedRuns = 0;
    bool firstRefused = false;
    bool secondRefused = false;
    HostCwTextEngine engine(&rig.model, [&](const TxCoordinator::Request&, bool refused) {
        ++endedRuns;
        (endedRuns == 1 ? firstRefused : secondRefused) = refused;
    });
    constexpr int kWpm = 20;
    const auto wallMs = [&rig] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - rig.t0).count();
    };
    const auto pump = [&](std::int64_t untilMs) {
        while (wallMs() < untilMs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            rig.buildTo(wallMs() - 5);
        }
    };
    const std::int64_t startMs = wallMs();
    if (refusal) {
        // "EE": the first dit (0-60 ms) keys; the second is due at 240 ms.
        check(engine.play(sender, QStringLiteral("EE"), kWpm, {}), name + "the first message plays");
        pump(startMs + 150);
        setMode(QStringLiteral("USB"));
        pump(startMs + 320);
        setMode(QStringLiteral("CW"));
        check(endedRuns == 1 && firstRefused && notices == 1,
              name + "its second key-down is refused with a notice, and the run ends ("
                  + std::to_string(notices) + " notices)");
    } else {
        check(engine.play(sender, QStringLiteral("E"), kWpm, {}), name + "the first message plays");
        pump(startMs + 150);
        const bool pending = engine.retiring() && HostCwTextEngineTestAccess::inputPending(engine);
        HostCwTextEngineTestAccess::expireRetireWait(engine);
        // Manual MOX has no hang: the input ends once the release is written,
        // so the run is already fenced and the cap finds nothing to fence.
        check(endedRuns == 1 && !firstRefused && pending == breakIn && !engine.retiring(),
              name + (breakIn ? "the drained run's input is pending in the hang, and the cap fences it"
                              : "the drained run's input ended with its release, and it is fenced"));
    }
    pump(startMs + 350);
    const bool keyed = Hl2TxGateTestAccess::keyed(*rig.backend);
    const bool wire = Hl2TxGateTestAccess::wireKeyed(*rig.backend);
    check(keyed && wire == !breakIn,
          name + "the second text falls inside the hang, with MOX at the writer "
              + (wire ? "kept" : "dropped by the fence"));
    const int noticesBefore = notices;
    const std::int64_t secondMs = wallMs();
    check(engine.play(sender, QStringLiteral("E"), kWpm, {}), name + "the second message plays");
    pump(secondMs + 800);
    std::vector<Run> later;
    for (const Run& r : carriers(rig.iTrace)) {
        if (r.start >= samplesOfMs(secondMs)) {
            later.push_back(r);
        }
    }
    check((secondRefused && notices > noticesBefore)
              || (!secondRefused && later.size() == 1 && within(later[0].length, samplesOfMs(1200 / kWpm), 2)),
          name + "the second dit is whole on EP2, or refused with a notice (" + runsText(later) + "; "
              + (secondRefused ? "refused" : "not refused") + "; " + Hl2TxGateTestAccess::counters(*rig.backend)
              + ")");
    if (!breakIn) {
        rig.model.transmitModel().setMox(false);
        Rig::settle();
    }
}

// #7568: manual MOX, the paddles go idle after one element while the
// operator still holds MOX. The element's key-up completion ends the last
// CW intent; the 5 ms fall it started must still reach zero on EP2, not step.
// Each edge reaches the model at its own instant, and the model runs right
// after the frame that writes the release, so a completion lands in the fall.
// `upAfterRelease`: the key-up reaches the I/O thread only once the release
// is on EP2 (a stalled GUI thread), inside the fall.
static void testManualMoxLastElementFallIsWhole(bool upAfterRelease)
{
    const std::string name = std::string("manual MOX, paddles idle, MOX held")
        + (upAfterRelease ? ", key-up after its release is written" : "") + ": ";
    Rig rig(false);
    rig.model.transmitModel().setMox(true);
    Rig::settle();
    const TxCoordinator::Producer paddle = rig.model.registerTxProducer();
    const TxCoordinator::Request element = paddle.request().derive();
    rig.model.queueProducerCwKeyEdge(element, true, QStringLiteral("test:paddle"), 0, 0, rig.at(100), false,
                                     [&rig] { ++rig.refusedDowns; }, rig.at(140));
    Rig::settle();
    const std::int64_t up = samplesOfMs(140) + samplesOfMs(Hl2Settings::kCwEdgeLeadDefaultMs);
    const auto buildThrough = [&rig](std::int64_t sample) {
        while (static_cast<std::int64_t>(rig.iTrace.size()) <= sample) {
            appendI(Hl2TxGateTestAccess::packet(*rig.backend), rig.iTrace);
        }
    };
    if (upAfterRelease) {
        buildThrough(up);
    } else {
        rig.buildTo(140);
    }
    rig.model.queueProducerCwKeyEdge(element, false, QStringLiteral("test:paddle"), 0, 0, rig.at(140));
    Rig::settle();
    buildThrough(up);
    Rig::settle();
    for (int i = 0; i < 4; ++i) {   // frame by frame through the fall, the model running between
        appendI(Hl2TxGateTestAccess::packet(*rig.backend), rig.iTrace);
        Rig::settle();
    }
    rig.buildTo(400);
    Rig::settle();   // the completion, finished at the fall's end, reaches the model
    const std::int64_t fall = samplesOfMs(5);
    bool monotonic = true;
    int largestStep = 0;
    for (std::int64_t n = up + 1; n <= up + fall; ++n) {
        const int step = rig.iTrace[static_cast<std::size_t>(n - 1)] - rig.iTrace[static_cast<std::size_t>(n)];
        monotonic = monotonic && step >= 0;
        largestStep = std::max(largestStep, step);
    }
    std::string trace;
    for (std::int64_t n = up; n <= up + fall; n += 20) {
        trace += " " + std::to_string(rig.iTrace[static_cast<std::size_t>(n)]);
    }
    check(rig.refusedDowns == 0 && monotonic && largestStep < 400
              && rig.iTrace[static_cast<std::size_t>(up + fall - 20)] > 0
              && rig.iTrace[static_cast<std::size_t>(up + fall)] == 0,
          name + "the element's 5 ms fall decreases monotonically to 0 (I every 20 samples:" + trace
              + "; largest step " + std::to_string(largestStep) + ")");
    check(!rig.model.txProducerInputPending(paddle), name + "the element's intent ends after its fall");
    check(Hl2TxGateTestAccess::keyed(*rig.backend), name + "the operator's MOX still keys");
    rig.model.transmitModel().setMox(false);
    Rig::settle();
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
    testSuppressedStraightUpAfterTheLastRelease();
    testRevokedProducerAtTheWriter();
    testHostTextFallSurvivesTheNextDown();
    testManualMoxLastElementFallIsWhole(false);
    testManualMoxLastElementFallIsWhole(true);
    testHostTextEngineLastElementIsWhole(20, true);
    testHostTextEngineLastElementIsWhole(40, true);
    testHostTextEngineLastElementIsWhole(20, false);
    testHostTextSameSenderNextTextIsSpaced(80, true, false);
    testHostTextSameSenderNextTextIsSpaced(80, false, false);
    testHostTextSameSenderNextTextIsSpaced(200, true, false);
    testHostTextSameSenderNextTextIsSpaced(400, true, false);
    testHostTextSameSenderNextTextIsSpaced(150, true, true);
    testHostTextFencedInTheHangNextTextIsOnTheWire(true);
    testHostTextFencedInTheHangNextTextIsOnTheWire(false);
    testRekeyAfterAFenceIsOneIoAction();
    testRefusedRekeyAfterAFenceRaisesNoMox();
    testRekeyInALiveOverLeavesItsMoxAlone();
    testHostTextOtherFenceInTheHang(HangFence::Refusal, true);
    testHostTextOtherFenceInTheHang(HangFence::Refusal, false);
    testHostTextOtherFenceInTheHang(HangFence::RetireCap, true);
    testHostTextOtherFenceInTheHang(HangFence::RetireCap, false);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
