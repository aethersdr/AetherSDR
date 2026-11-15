// ByteRelay pump/state-machine proof with injected transports: no sockets.
// Every case compares complete byte streams, so a dropped, duplicated,
// reordered or rewritten byte fails regardless of how reads were chunked.

#include "core/ByteRelay.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <vector>

using AetherSDR::ByteRelay;
using AetherSDR::ByteRelayEndpoint;
using Side = ByteRelay::Side;

namespace {

int g_failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void spin(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 2000)
{
    QElapsedTimer t;
    t.start();
    while (!predicate() && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return predicate();
}

// Scripted transport. Readable bytes sit in `inbox`; accepted writes land in
// `unsent` until flush() hands them to `delivered` (modelling the OS taking
// them), which is when a real socket would emit bytesWritten.
class FakeEndpoint : public ByteRelayEndpoint {
public:
    QByteArray inbox;
    QByteArray unsent;
    QByteArray delivered;
    std::vector<qint64> acceptScript;  // per write() call; consumed front-first
    qint64 acceptLimit{-1};            // after the script: -1 = accept all
    bool autoFlush{true};
    bool failWrites{false};
    bool gracefulClose{false};
    bool aborted{false};
    qint64 maxReadRequest{0};
    int writeCalls{0};

    qint64 bytesAvailable() const override { return inbox.size(); }
    QByteArray read(qint64 maxBytes) override
    {
        maxReadRequest = std::max(maxReadRequest, maxBytes);
        const QByteArray out = inbox.left(maxBytes);
        inbox.remove(0, out.size());
        return out;
    }
    qint64 write(const QByteArray& data) override
    {
        ++writeCalls;
        if (failWrites || aborted) {
            return -1;
        }
        qint64 n = data.size();
        if (!acceptScript.empty()) {
            n = std::min<qint64>(n, acceptScript.front());
            acceptScript.erase(acceptScript.begin());
        } else if (acceptLimit >= 0) {
            n = std::min<qint64>(n, acceptLimit);
        }
        if (autoFlush) {
            delivered += data.left(n);
        } else {
            unsent += data.left(n);
        }
        return n;
    }
    qint64 bytesToWrite() const override { return unsent.size(); }
    void closeGracefully() override { gracefulClose = true; }
    void abort() override { aborted = true; }

    qint64 flush(qint64 n)
    {
        n = std::min<qint64>(n, unsent.size());
        delivered += unsent.left(n);
        unsent.remove(0, n);
        return n;
    }
};

struct Rig {
    FakeEndpoint a;
    FakeEndpoint b;
    std::unique_ptr<ByteRelay> relay;
    bool finished{false};
    ByteRelay::EndReason reason{ByteRelay::EndReason::None};
    qint64 discardedA{-1};
    qint64 discardedB{-1};

    explicit Rig(const ByteRelay::Limits& limits = {})
    {
        relay = std::make_unique<ByteRelay>(&a, &b, limits);
        QObject::connect(relay.get(), &ByteRelay::finished, relay.get(),
                         [this](ByteRelay::EndReason r, const QString&, qint64 da, qint64 db) {
            finished = true;
            reason = r;
            discardedA = da;
            discardedB = db;
        });
    }
};

QByteArray allByteValues(int repeats)
{
    QByteArray out;
    for (int r = 0; r < repeats; ++r) {
        for (int v = 0; v < 256; ++v) {
            out.append(static_cast<char>(v));
        }
        out.append("\r\n\n\r\0", 5);
    }
    return out;
}

QByteArray randomBytes(int size, unsigned seed)
{
    std::mt19937 rng(seed);
    QByteArray out(size, '\0');
    for (int i = 0; i < size; ++i) {
        out[i] = static_cast<char>(rng() & 0xFF);
    }
    return out;
}

void testAllByteValuesBothDirections()
{
    Rig rig;
    const QByteArray down = allByteValues(4);
    const QByteArray up = allByteValues(3) + QByteArray("unknown command\x00\xff", 17);
    rig.relay->start();
    rig.a.inbox = down;
    rig.relay->notifyReadable(Side::A);
    rig.b.inbox = up;
    rig.relay->notifyReadable(Side::B);
    check(rig.b.delivered == down, "A->B carries every byte value, CR/LF/NUL unchanged");
    check(rig.a.delivered == up, "B->A carries every byte value unchanged");
    check(rig.relay->stats().forwarded[0] == static_cast<quint64>(down.size()),
          "A->B counter equals bytes delivered");
    check(rig.relay->stats().forwarded[1] == static_cast<quint64>(up.size()),
          "B->A counter equals bytes delivered");
}

void testUpstreamFirstOutput()
{
    Rig rig;
    const QByteArray greeting = randomBytes(300, 7);
    rig.b.inbox = greeting;  // arrived before the relay started
    rig.relay->start();
    check(rig.a.delivered == greeting,
          "unsolicited upstream output is forwarded before the client sends anything");
    check(rig.b.delivered.isEmpty(), "nothing is synthesized toward upstream");
}

void testFragmentedAndCoalesced()
{
    ByteRelay::Limits limits;
    limits.directionBudget = 4096;
    limits.sliceBytes = 1024;
    limits.readChunk = 333;
    Rig rig(limits);
    rig.b.autoFlush = false;
    rig.a.autoFlush = false;
    rig.relay->start();

    const QByteArray down = randomBytes(200000, 11);
    const QByteArray up = randomBytes(150000, 12);
    std::mt19937 rng(99);
    int posDown = 0;
    int posUp = 0;
    while ((rig.b.delivered.size() < down.size() || rig.a.delivered.size() < up.size())
           && g_failures == 0) {
        // Arrivals in random fragment sizes, sometimes several before a notify.
        const int arrivals = 1 + static_cast<int>(rng() % 3);
        for (int i = 0; i < arrivals; ++i) {
            const int nd = std::min<int>(static_cast<int>(rng() % 1500) + 1, down.size() - posDown);
            rig.a.inbox += down.mid(posDown, nd);
            posDown += nd;
            const int nu = std::min<int>(static_cast<int>(rng() % 900) + 1, up.size() - posUp);
            rig.b.inbox += up.mid(posUp, nu);
            posUp += nu;
        }
        rig.relay->notifyReadable(Side::A);
        rig.relay->notifyReadable(Side::B);
        check(rig.relay->stats().queued[0] <= limits.directionBudget, "A->B stays within budget");
        check(rig.relay->stats().queued[1] <= limits.directionBudget, "B->A stays within budget");
        // Departures in different random sizes.
        if (rig.b.flush(static_cast<qint64>(rng() % 2000)) > 0) {
            rig.relay->notifyBytesWritten(Side::B);
        }
        if (rig.a.flush(static_cast<qint64>(rng() % 2000)) > 0) {
            rig.relay->notifyBytesWritten(Side::A);
        }
        QCoreApplication::processEvents();
    }
    check(rig.b.delivered == down, "fragmented A->B stream arrives complete and in order");
    check(rig.a.delivered == up, "fragmented B->A stream arrives complete and in order");
    check(rig.a.maxReadRequest <= limits.readChunk && rig.b.maxReadRequest <= limits.readChunk,
          "reads never exceed the read chunk");
}

void testPartialAndZeroWrites()
{
    Rig rig;
    rig.b.acceptScript = {3, 0, 1, 0, 0, 5, 2};
    rig.b.acceptLimit = 0;  // after the script, refuse until told otherwise
    rig.relay->start();
    const QByteArray data = randomBytes(5000, 21);
    rig.a.inbox = data;
    rig.relay->notifyReadable(Side::A);
    check(rig.b.delivered == data.left(3), "partial write keeps exactly the accepted prefix");
    for (int i = 0; i < 6; ++i) {
        rig.relay->notifyBytesWritten(Side::B);
    }
    check(rig.b.delivered == data.left(11), "zero writes neither drop nor duplicate bytes");
    check(rig.relay->stats().queued[0] <= ByteRelay::Limits{}.directionBudget,
          "pending stays bounded while the destination refuses");
    rig.b.acceptLimit = 7;  // dribble
    for (int i = 0; i < 2000 && rig.b.delivered.size() < data.size(); ++i) {
        rig.relay->notifyBytesWritten(Side::B);
        QCoreApplication::processEvents();
    }
    check(rig.b.delivered == data, "stream completes intact through partial writes");
}

void testBackpressureAndBounds()
{
    ByteRelay::Limits limits;
    limits.directionBudget = 1024;
    limits.sliceBytes = 64 * 1024;
    Rig rig(limits);
    rig.b.autoFlush = false;  // destination never drains on its own
    rig.relay->start();
    const QByteArray data = randomBytes(1 << 20, 31);
    rig.a.inbox = data;
    rig.relay->notifyReadable(Side::A);
    QCoreApplication::processEvents();
    check(rig.b.unsent.size() == limits.directionBudget, "relay fills exactly up to the budget");
    check(rig.a.inbox.size() == data.size() - limits.directionBudget,
          "source is not read once the direction is full (backpressure)");
    rig.relay->notifyReadable(Side::A);
    check(rig.a.inbox.size() == data.size() - limits.directionBudget,
          "repeated readable notifications do not grow the queue");

    while (rig.b.delivered.size() < data.size()) {
        rig.b.flush(700);
        rig.relay->notifyBytesWritten(Side::B);
        check(rig.relay->stats().queued[0] <= limits.directionBudget, "queue bounded while draining");
        if (g_failures) {
            break;
        }
    }
    check(rig.b.delivered == data, "backpressured stream completes intact");
}

void testSliceYieldsToEventLoop()
{
    ByteRelay::Limits limits;
    limits.sliceBytes = 4096;
    limits.readChunk = 1024;
    Rig rig(limits);
    rig.relay->start();
    const QByteArray data = randomBytes(64 * 1024, 41);
    rig.a.inbox = data;
    rig.relay->notifyReadable(Side::A);
    check(rig.b.delivered.size() == limits.sliceBytes,
          "one pump moves at most one slice before yielding");
    check(waitUntil([&] { return rig.b.delivered.size() == data.size(); }),
          "queued continuation finishes the transfer without new readiness signals");
    check(rig.b.delivered == data, "sliced transfer is intact");
}

void testProgressTimeoutOnlyWhilePending()
{
    ByteRelay::Limits limits;
    limits.progressTimeoutMs = 40;
    {
        Rig idle(limits);
        idle.relay->start();
        spin(150);
        check(!idle.finished, "an idle, empty relay never times out");
    }
    Rig rig(limits);
    rig.b.autoFlush = false;
    rig.relay->start();
    rig.a.inbox = randomBytes(5000, 51);
    rig.relay->notifyReadable(Side::A);
    check(waitUntil([&] { return rig.finished; }, 1000), "stalled pending bytes time out");
    check(rig.reason == ByteRelay::EndReason::ProgressTimeout, "timeout reason is reported");
    check(rig.a.aborted && rig.b.aborted, "timeout hard-closes both sides");
    check(rig.discardedA == 5000, "discarded count includes the unsent bytes");
}

void testStopDiscardsAndIgnoresLateCallbacks()
{
    ByteRelay::Limits limits;
    limits.sliceBytes = 1000;
    limits.readChunk = 1000;
    Rig rig(limits);
    rig.relay->start();
    rig.a.inbox = randomBytes(10000, 61);
    rig.relay->notifyReadable(Side::A);  // schedules a continuation
    check(rig.b.delivered.size() == 1000, "first slice delivered");
    rig.relay->stop();
    check(rig.finished && rig.reason == ByteRelay::EndReason::Stopped, "stop reports Stopped");
    check(rig.a.aborted && rig.b.aborted, "stop hard-closes both sides");
    const int before = rig.a.inbox.size();
    QCoreApplication::processEvents();
    rig.relay->notifyReadable(Side::A);
    rig.relay->notifyBytesWritten(Side::B);
    rig.relay->notifyEndOfInput(Side::A);
    check(rig.a.inbox.size() == before && rig.b.delivered.size() == 1000,
          "no continuation or late notification moves bytes after stop");
}

void testGenerationIsolation()
{
    Rig first;
    first.b.autoFlush = false;
    first.relay->start();
    first.a.inbox = QByteArray("old-connection-bytes");
    first.relay->notifyReadable(Side::A);
    first.relay->stop();

    Rig second;
    second.relay->start();
    // A late callback from the first pair must not touch the second.
    first.relay->notifyReadable(Side::A);
    first.relay->notifyBytesWritten(Side::B);
    second.a.inbox = QByteArray("new");
    second.relay->notifyReadable(Side::A);
    check(second.b.delivered == QByteArray("new"),
          "a new pair starts empty; unsent data never enters another connection");
}

void testOrderlyEofDrain()
{
    ByteRelay::Limits limits;
    limits.directionBudget = 512;
    Rig rig(limits);
    rig.b.autoFlush = false;
    rig.relay->start();
    const QByteArray tail = randomBytes(4000, 71);
    rig.a.inbox = tail;  // final bytes plus FIN in the same delivery
    rig.relay->notifyReadable(Side::A);
    rig.relay->notifyEndOfInput(Side::A);
    check(!rig.finished, "drain waits for buffered tail bytes");
    check(!rig.a.gracefulClose, "pair is not closed while the tail is still queued");
    while (!rig.b.unsent.isEmpty() || !rig.a.inbox.isEmpty()) {
        rig.b.flush(300);
        rig.relay->notifyBytesWritten(Side::B);
        if (g_failures) {
            break;
        }
    }
    check(rig.b.delivered == tail, "every byte read before EOF is forwarded");
    check(rig.a.gracefulClose && rig.b.gracefulClose, "pair closes gracefully after the drain");
    check(!rig.a.aborted && !rig.b.aborted, "orderly close does not abort");
    rig.relay->notifyClosed(Side::B);
    check(rig.finished && rig.reason == ByteRelay::EndReason::PeerClosed, "drain completes as PeerClosed");
    check(rig.discardedA == 0, "nothing from the closing side was discarded");
}

void testEofDrainTimeoutAndReverseDiscard()
{
    ByteRelay::Limits limits;
    limits.directionBudget = 512;
    limits.drainTimeoutMs = 40;
    Rig rig(limits);
    rig.b.autoFlush = false;
    rig.a.autoFlush = false;
    rig.relay->start();
    rig.b.inbox = randomBytes(100, 81);  // B->A queued, never flushed
    rig.relay->notifyReadable(Side::B);
    rig.a.inbox = randomBytes(2000, 82);
    rig.relay->notifyReadable(Side::A);
    rig.relay->notifyEndOfInput(Side::A);
    check(waitUntil([&] { return rig.finished; }, 1000), "a stuck drain is bounded");
    check(rig.reason == ByteRelay::EndReason::DrainTimeout, "drain timeout reason reported");
    check(rig.a.aborted && rig.b.aborted, "drain timeout hard-closes the pair");
    check(rig.discardedB == 100, "reverse-direction bytes are counted as discarded");
    check(rig.discardedA > 0, "undelivered tail bytes are counted, not claimed delivered");
}

void testWriteErrorIsHard()
{
    Rig rig;
    rig.b.failWrites = true;
    rig.relay->start();
    rig.a.inbox = QByteArray("x");
    rig.relay->notifyReadable(Side::A);
    check(rig.finished && rig.reason == ByteRelay::EndReason::TransportError,
          "a failed write ends the relay as a transport error");
    check(rig.a.aborted && rig.b.aborted, "transport error closes both sides");
    check(rig.a.delivered.isEmpty(), "no error text is injected into the other stream");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testAllByteValuesBothDirections();
    testUpstreamFirstOutput();
    testFragmentedAndCoalesced();
    testPartialAndZeroWrites();
    testBackpressureAndBounds();
    testSliceYieldsToEventLoop();
    testProgressTimeoutOnlyWhilePending();
    testStopDiscardsAndIgnoresLateCallbacks();
    testGenerationIsolation();
    testOrderlyEofDrain();
    testEofDrainTimeoutAndReverseDiscard();
    testWriteErrorIsHard();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("byte_relay_test: all checks passed\n");
    return 0;
}
