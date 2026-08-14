// Contract tests for the backend-neutral IQ provider (#4955).
//
// The acceptance criteria this file exists to hold down:
//
//   * absolute above/below-centre tone tests catch MIRRORED IQ, on the sim
//     reference, on the HL2 adapter (whose wire is conjugated) and on the Flex
//     adapter (whose payload is little-endian);
//   * the reported centre is the DDC/pan centre, not an offset slice VFO;
//   * requested and achieved rates are represented separately;
//   * several clients share one physical endpoint without premature teardown;
//   * stopping one of four subscriptions leaves the other three running;
//   * receiver churn and disconnect INVALIDATE rather than silently rebind;
//   * backpressure is bounded and drops are counted.
//
// Handedness is measured absolutely, never by loopback: two conjugation errors
// at opposite ends of a loop cancel and the test passes on mirrored IQ. The
// measure here is the phase advance between consecutive samples — for a tone at
// frequency f, arg(x[n+1] · conj(x[n])) = 2πf/fs, whose SIGN is the handedness
// and nothing else.

#include <QCoreApplication>
#include <QByteArray>
#include <QSignalSpy>
#include <QtEndian>

#include <cmath>
#include <complex>
#include <cstring>
#include <vector>

#include "core/backends/flex/FlexIqProvider.h"
#include "core/IqProvider.h"
#include "core/SimIqProvider.h"
#include "core/backends/hl2/Hl2IqProvider.h"
#include "models/DaxIqModel.h"

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool cond, const char* what)
{
    if (!cond) {
        ++g_failures;
        qWarning("FAIL: %s", what);
    } else {
        qInfo("ok: %s", what);
    }
}

constexpr double kPi = 3.14159265358979323846;

// The absolute handedness measure. Positive means the tone sits ABOVE the
// reported centre in the canonical convention.
double meanPhaseAdvance(const std::vector<IqSample>& s)
{
    if (s.size() < 2)
        return 0.0;
    std::complex<double> acc{0.0, 0.0};
    for (std::size_t n = 1; n < s.size(); ++n)
        acc += std::complex<double>(s[n]) * std::conj(std::complex<double>(s[n - 1]));
    return std::arg(acc);
}

// Collects blocks for one endpoint.
struct Sink : public QObject {
    QHash<QString, std::vector<IqBlockPtr>> byEndpoint;
    int total{0};

    void attach(IqProvider* p)
    {
        QObject::connect(p, &IqProvider::iqBlockReady, this,
                         [this](const QString& id, IqBlockPtr b) {
            byEndpoint[id].push_back(b);
            ++total;
        });
    }
    void clear() { byEndpoint.clear(); total = 0; }
};

void pump()
{
    // The provider hands blocks over through a queued wake, deliberately, so
    // the backend I/O thread never runs a consumer. Draining it is one turn of
    // the event loop.
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

// A wire tone as the HL2 actually delivers it: HPSDR IQ is the CONJUGATE of the
// analytic convention, so RF above the NCO arrives at a negative frequency.
std::vector<std::complex<float>> hpsdrWireTone(double rfOffsetHz, int rateHz, int n)
{
    std::vector<std::complex<float>> out(static_cast<std::size_t>(n));
    const double w = -2.0 * kPi * rfOffsetHz / rateHz;   // the conjugation
    for (int i = 0; i < n; ++i) {
        out[static_cast<std::size_t>(i)] =
            std::complex<float>(static_cast<float>(std::cos(w * i)),
                                static_cast<float>(std::sin(w * i)));
    }
    return out;
}

// A Flex dax_iq payload: LITTLE-endian float32, interleaved I/Q, already in the
// analytic convention.
QByteArray flexLePayload(double rfOffsetHz, int rateHz, int n)
{
    QByteArray out(n * 2 * 4, Qt::Uninitialized);
    char* p = out.data();
    const double w = 2.0 * kPi * rfOffsetHz / rateHz;
    for (int i = 0; i < n; ++i) {
        const float fi = static_cast<float>(std::cos(w * i));
        const float fq = static_cast<float>(std::sin(w * i));
        quint32 ri = 0, rq = 0;
        std::memcpy(&ri, &fi, 4);
        std::memcpy(&rq, &fq, 4);
        ri = qToLittleEndian(ri);
        rq = qToLittleEndian(rq);
        std::memcpy(p + (i * 2) * 4, &ri, 4);
        std::memcpy(p + (i * 2 + 1) * 4, &rq, 4);
    }
    return out;
}

// ── the sim reference ────────────────────────────────────────────────────────

void testSimHandednessAndCenter()
{
    SimIqProvider p;
    // The centre is the DDC/PAN centre. The slice a client is tuned to sits at
    // 7'074'000 — 4 kHz above it — and must never be what gets reported: a
    // skimmer told the slice mis-maps every spot by exactly that offset.
    p.setEndpoints({{QStringLiteral("pan-a"), 7'070'000}});
    Sink sink;
    sink.attach(&p);

    const IqLease l = p.acquire(QStringLiteral("pan-a"), &sink);
    check(l.granted, "sim: lease granted");
    check(l.centerHz == 7'070'000, "sim: lease reports the pan centre, not the slice VFO");

    p.emitTone(QStringLiteral("pan-a"), +5000.0, 4096);
    pump();
    check(sink.byEndpoint.value(QStringLiteral("pan-a")).size() == 1,
          "sim: one block delivered");
    const auto& b = sink.byEndpoint[QStringLiteral("pan-a")].front();
    check(b->centerHz == 7'070'000, "sim: block carries the pan centre");
    check(meanPhaseAdvance(b->samples) > 0.0,
          "sim: a tone ABOVE centre has positive complex frequency");

    sink.clear();
    p.emitTone(QStringLiteral("pan-a"), -5000.0, 4096);
    pump();
    check(meanPhaseAdvance(sink.byEndpoint[QStringLiteral("pan-a")].front()->samples) < 0.0,
          "sim: a tone BELOW centre has negative complex frequency");

    p.release(QStringLiteral("pan-a"), &sink);
}

void testRequestedVersusAchieved()
{
    SimIqProvider p;
    p.setEndpoints({{QStringLiteral("pan-a"), 14'100'000}});
    Sink sink;

    // A per-stream backend honours a supported request.
    p.setRatePolicy({48000, 96000, 192000}, 48000, IqRateTopology::PerStream, false);
    IqLease l = p.acquire(QStringLiteral("pan-a"), &sink, 96000);
    check(l.granted && l.achievedRateHz == 96000,
          "rate: per-stream backend achieves a supported request");

    // An UNSUPPORTED request must report what is running rather than echo the
    // ask. A skimmer that believes a rate it is not receiving decodes at the
    // wrong speed and reports nothing at all.
    l = p.requestRate(QStringLiteral("pan-a"), 44100);
    check(l.granted && l.achievedRateHz == 96000,
          "rate: unsupported request reports the achieved rate, not the request");
    p.release(QStringLiteral("pan-a"), &sink);

    // A radio-wide shared rate must not be moved because a skimmer asked: on an
    // HL2 that re-spans every panadapter and rebuilds every receive DSP chain.
    SimIqProvider shared;
    shared.setEndpoints({{QStringLiteral("hl2-0"), 7'100'000}});
    shared.setRatePolicy({48000, 96000, 192000}, 192000,
                         IqRateTopology::SharedAcrossReceivers, true);
    l = shared.acquire(QStringLiteral("hl2-0"), &sink, 48000);
    check(l.granted && l.achievedRateHz == 192000,
          "rate: shared-rate backend keeps its rate and reports it honestly");
    check(shared.capabilities().rateChangeIsRadioWide,
          "rate: shared-rate backend declares the change is radio-wide");
    shared.release(QStringLiteral("hl2-0"), &sink);
}

void testSharedLeaseAndFourReceivers()
{
    SimIqProvider p;
    p.setEndpoints({{QStringLiteral("p0"), 1}, {QStringLiteral("p1"), 2},
                    {QStringLiteral("p2"), 3}, {QStringLiteral("p3"), 4}});
    p.setCapacity(4);

    int clientA = 0, clientB = 0;   // two identities, addresses only
    check(p.acquire(QStringLiteral("p0"), &clientA).granted, "share: client A leases p0");
    check(p.acquire(QStringLiteral("p0"), &clientB).granted, "share: client B leases p0");
    check(p.opens() == 1, "share: one physical open for two subscribers");
    check(p.subscriberCount(QStringLiteral("p0")) == 2, "share: refcount is 2");

    p.release(QStringLiteral("p0"), &clientA);
    check(p.closes() == 0, "share: the FIRST release does not tear the endpoint down");
    check(p.subscriberCount(QStringLiteral("p0")) == 1, "share: refcount is 1");
    p.release(QStringLiteral("p0"), &clientB);
    check(p.closes() == 1, "share: the LAST release closes it (#4951)");

    // Four concurrent receivers; stopping one must leave the other three alone.
    Sink sink;
    sink.attach(&p);
    for (const char* id : {"p0", "p1", "p2", "p3"})
        check(p.acquire(QString::fromLatin1(id), &sink).granted, "four: lease granted");

    p.release(QStringLiteral("p1"), &sink);
    sink.clear();
    for (const char* id : {"p0", "p1", "p2", "p3"})
        p.emitTone(QString::fromLatin1(id), 1000.0, 256);
    pump();
    check(sink.byEndpoint.value(QStringLiteral("p1")).empty(),
          "four: the stopped receiver delivers nothing");
    check(!sink.byEndpoint.value(QStringLiteral("p0")).empty()
              && !sink.byEndpoint.value(QStringLiteral("p2")).empty()
              && !sink.byEndpoint.value(QStringLiteral("p3")).empty(),
          "four: the other three are uninterrupted");
}

void testChurnAndReconnectCannotRebind()
{
    SimIqProvider p;
    p.setEndpoints({{QStringLiteral("rx-a"), 7'100'000},
                    {QStringLiteral("rx-b"), 14'100'000}});
    Sink sink;
    sink.attach(&p);
    QSignalSpy invalidated(&p, &IqProvider::leaseInvalidated);

    check(p.acquire(QStringLiteral("rx-a"), &sink).granted, "churn: leased rx-a on 40 m");

    // rx-a goes away and rx-b takes its wire slot. The subscription must END,
    // not follow the slot onto 20 m.
    p.removeEndpoint(QStringLiteral("rx-a"));
    check(invalidated.count() == 1, "churn: the vanished receiver invalidates its lease");
    check(p.subscriberCount(QStringLiteral("rx-a")) == 0, "churn: no lease survives");
    check(p.subscriberCount(QStringLiteral("rx-b")) == 0,
          "churn: the subscription is NOT rebound to the receiver that took the slot");

    // A disconnect does the same for every endpoint.
    p.setEndpoints({{QStringLiteral("rx-b"), 14'100'000}});
    check(p.acquire(QStringLiteral("rx-b"), &sink).granted, "churn: leased rx-b");
    p.setConnected(false);
    check(p.subscriberCount(QStringLiteral("rx-b")) == 0,
          "churn: disconnect invalidates every lease");
    check(!p.acquire(QStringLiteral("rx-b"), &sink).granted,
          "churn: a disconnected provider refuses rather than pretending");
}

void testBoundedBackpressure()
{
    SimIqProvider p;
    p.setEndpoints({{QStringLiteral("pan-a"), 1'000'000}});
    Sink sink;
    sink.attach(&p);
    p.acquire(QStringLiteral("pan-a"), &sink);

    // Produce far more than the queue can hold WITHOUT draining: this is the
    // radio's I/O thread outrunning the GUI, and it must never block or grow
    // without bound.
    const int produced = IqProvider::kMaxQueuedBlocks * 4;
    for (int i = 0; i < produced; ++i)
        p.emitTone(QStringLiteral("pan-a"), 1000.0, 64);

    check(p.droppedBlocks(QStringLiteral("pan-a")) > 0,
          "backpressure: overflow is DROPPED and counted, not queued");
    pump();
    const auto& got = sink.byEndpoint[QStringLiteral("pan-a")];
    check(static_cast<int>(got.size()) <= IqProvider::kMaxQueuedBlocks,
          "backpressure: no more than the bound is ever delivered");
    check(!got.empty() && got.back()->sequence == static_cast<quint64>(produced - 1),
          "backpressure: the NEWEST block survives (drop-oldest)");
    bool sawDiscontinuity = false;
    for (const auto& b : got) {
        if (b->droppedBefore > 0) { sawDiscontinuity = true; break; }
    }
    check(sawDiscontinuity, "backpressure: a discontinuity marker reaches the consumer");

    // After the last release nothing more can arrive.
    sink.clear();
    p.release(QStringLiteral("pan-a"), &sink);
    p.emitTone(QStringLiteral("pan-a"), 1000.0, 64);
    pump();
    check(sink.total == 0, "teardown: no callback after the lease is released");
}

// ── the HL2 adapter: the conjugation that would mirror every band ────────────

void testHl2Handedness()
{
    Hl2IqProvider p;
    p.publishRoutes({{QStringLiteral("hl2-0"), 7'070'000}}, 192000, 4);
    p.setConnected(true);
    Sink sink;
    sink.attach(&p);

    const IqLease l = p.acquire(QStringLiteral("hl2-0"), &sink);
    check(l.granted, "hl2: lease granted");
    check(l.achievedRateHz == 192000, "hl2: achieved rate is the shared DDC rate");
    check(l.centerHz == 7'070'000, "hl2: centre is the DDC NCO");

    // RF 5 kHz ABOVE the NCO, delivered the way the wire actually delivers it.
    p.feedWireBlock(0, hpsdrWireTone(+5000.0, 192000, 4096));
    pump();
    const auto& blocks = sink.byEndpoint[QStringLiteral("hl2-0")];
    check(blocks.size() == 1, "hl2: one canonical block delivered");
    check(meanPhaseAdvance(blocks.front()->samples) > 0.0,
          "hl2: wire IQ is conjugated, so RF above the NCO is POSITIVE "
          "(unconjugated, every skimmer band would be mirrored)");

    sink.clear();
    p.feedWireBlock(0, hpsdrWireTone(-5000.0, 192000, 4096));
    pump();
    check(meanPhaseAdvance(sink.byEndpoint[QStringLiteral("hl2-0")].front()->samples) < 0.0,
          "hl2: RF below the NCO is negative");

    // A stop detaches the tap; it must not be able to tear the receiver down,
    // and the provider must keep the endpoint (the panadapter is still using it).
    p.release(QStringLiteral("hl2-0"), &sink);
    check(p.endpoints().size() == 1,
          "hl2: releasing the tap leaves the receiver and its panadapter running");
}

void testHl2LinkBudget()
{
    Hl2IqProvider p;
    // Four receivers at 384 kHz is the case the EP6 link budget refuses. The
    // provider must REPORT that rather than overrun the wire — a dropped EP6
    // packet is a simultaneous gap in every receiver.
    p.publishRoutes({{QStringLiteral("hl2-0"), 1}, {QStringLiteral("hl2-1"), 2},
                     {QStringLiteral("hl2-2"), 3}, {QStringLiteral("hl2-3"), 4}},
                    384000, 4);
    p.setConnected(true);
    int client = 0;
    const IqLease refused = p.acquire(QStringLiteral("hl2-0"), &client);
    check(!refused.granted && refused.refusal == IqRefusal::BackendRefused,
          "hl2: four receivers at 384 kHz is refused, with a reason");
    check(p.capabilities().maxConcurrentEndpoints < 4,
          "hl2: capacity at 384 kHz is reported as fewer than four");

    // The same four at 192 kHz fit, which is what makes four concurrent SDC
    // skimmers feasible on shipping four-DDC gateware.
    p.publishRoutes({{QStringLiteral("hl2-0"), 1}, {QStringLiteral("hl2-1"), 2},
                     {QStringLiteral("hl2-2"), 3}, {QStringLiteral("hl2-3"), 4}},
                    192000, 4);
    check(p.capabilities().maxConcurrentEndpoints == 4,
          "hl2: four receivers fit through 192 kHz");
    for (const char* id : {"hl2-0", "hl2-1", "hl2-2", "hl2-3"}) {
        check(p.acquire(QString::fromLatin1(id), &client).granted,
              "hl2: four concurrent IQ receivers at 192 kHz");
    }
}

// ── the Flex adapter: little-endian payload, no conjugation ──────────────────

void testFlexHandednessAndEndianness()
{
    DaxIqModel dax;
    FlexIqProvider p;
    p.setDaxIqModel(&dax);
    p.setPanResolvers([]() { return QStringList{QStringLiteral("pan-0")}; },
                      [](const QString&) -> long long { return 14'070'000; });
    QStringList sent;
    p.setCommandSink([&sent](const QString& c) { sent << c; });
    p.setConnected(true);

    Sink sink;
    sink.attach(&p);
    const IqLease l = p.acquire(QStringLiteral("pan-0"), &sink);
    check(l.granted, "flex: lease granted");
    check(l.centerHz == 14'070'000, "flex: centre is the pan centre (#4172)");
    // The binding command is the only reason IQ ever flows: daxiq_channel is a
    // Panadapter property, so without it the radio streams nothing.
    check(sent.size() == 1 && sent.first().contains(QStringLiteral("daxiq_channel=1")),
          "flex: the pan is bound to the DAX-IQ channel");

    // Channel 1, because that is what the lease allocated.
    p.feedRawIqPacket(1, flexLePayload(+5000.0, 48000, 2048), 48000);
    pump();
    const auto& blocks = sink.byEndpoint[QStringLiteral("pan-0")];
    check(blocks.size() == 1, "flex: one canonical block delivered");
    check(blocks.front()->samples.size() == 2048, "flex: I/Q pairs, not floats");
    check(meanPhaseAdvance(blocks.front()->samples) > 0.0,
          "flex: little-endian payload decoded, RF above the pan centre is POSITIVE "
          "(read big-endian this is denormal noise — #3913)");

    sink.clear();
    p.feedRawIqPacket(1, flexLePayload(-5000.0, 48000, 2048), 48000);
    pump();
    check(meanPhaseAdvance(sink.byEndpoint[QStringLiteral("pan-0")].front()->samples) < 0.0,
          "flex: RF below the pan centre is negative");

    // An unleased channel must not produce anything.
    sink.clear();
    p.feedRawIqPacket(3, flexLePayload(+5000.0, 48000, 256), 48000);
    pump();
    check(sink.total == 0, "flex: an unleased channel publishes nothing");

    // A disconnect ends the lease rather than leaving it pointed at a channel
    // the radio will hand to somebody else on reconnect.
    p.setConnected(false);
    check(p.subscriberCount(QStringLiteral("pan-0")) == 0,
          "flex: disconnect invalidates the lease");
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testSimHandednessAndCenter();
    testRequestedVersusAchieved();
    testSharedLeaseAndFourReceivers();
    testChurnAndReconnectCannotRebind();
    testBoundedBackpressure();
    testHl2Handedness();
    testHl2LinkBudget();
    testFlexHandednessAndEndianness();

    if (g_failures) {
        qWarning("iq_provider_test: %d failure(s)", g_failures);
        return 1;
    }
    qInfo("iq_provider_test: all checks passed");
    return 0;
}
