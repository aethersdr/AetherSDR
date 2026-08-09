// Tests for the AX.25 airtime model (Ax25LinkTiming.h) and the link-liveness
// behaviour it feeds in Ax25Connection: the idle-link poll (T3), bounded REJ
// recovery, retry-budget-only-on-progress, and the SABME refusal that lets an
// AX.25 v2.2 caller fall back to v2.0 immediately.
//
// The airtime half is pure arithmetic. The liveness half drives a bare
// Ax25Connection over a simulated air channel, same style as tnc_terminal_test.

#include "TestEventLoop.h"
#include "core/tnc/Ax25.h"
#include "core/tnc/Ax25Connection.h"
#include "core/tnc/Ax25LinkTiming.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QObject>
#include <QString>
#include <QVector>

#include <cstdio>

using namespace AetherSDR;
using AetherSDR::ax25::Address;
using AetherSDR::ax25::Frame;
using AetherSDR::ax25::FrameType;
using AetherSDR::ax25::LinkTimingProfile;

static int g_failures = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);\
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

namespace {

Address call(const char* text)
{
    auto a = Address::parse(QString::fromLatin1(text));
    return a ? *a : Address{};
}

// Spin the event loop until `predicate` holds or the deadline passes. Needed
// only by the T3 test, which is inherently about the passage of time. The
// waiting itself is tests/TestEventLoop.h. (#4693)
template <typename Predicate>
bool spinUntil(Predicate predicate, int timeoutMs)
{
    return AetherTest::waitFor(predicate, timeoutMs);
}

LinkTimingProfile hf300()
{
    return LinkTimingProfile::forBaud(300); // 80 preamble flags
}

LinkTimingProfile vhf1200()
{
    return LinkTimingProfile::forBaud(1200); // 64 preamble flags
}

} // namespace

// ---------------------------------------------------------------------------
// The airtime model
// ---------------------------------------------------------------------------

static void testFrameAirtime()
{
    // HF 300, paclen 64: 82 on-air bytes * 8 * 1.02 = 669 bits, plus 88 flags
    // * 8 = 704 bits, over 300 baud = 4577 ms.
    const int hfFrame = ax25::frameAirtimeMs(hf300(), 64);
    CHECK(hfFrame > 4500 && hfFrame < 4650, "HF 300 paclen-64 I-frame is ~4.58 s on the air");

    // A supervisory frame (no info field, no PID) is 17 bytes but still pays
    // the full preamble — which is why the preamble dominates the HF budget.
    const int hfAck = ax25::frameAirtimeMs(hf300(), -1);
    CHECK(hfAck > 2750 && hfAck < 2875, "HF 300 supervisory frame is ~2.81 s on the air");
    CHECK(hfAck > ax25::frameAirtimeMs(hf300(), 64) / 2,
          "on HF the preamble dominates: an empty ack costs more than half a full frame");

    // Same frame at 1200 baud is four times quicker.
    const int vhfFrame = ax25::frameAirtimeMs(vhf1200(), 128);
    CHECK(vhfFrame > 1400 && vhfFrame < 1550, "VHF 1200 paclen-128 I-frame is ~1.47 s");

    // Digipeater hops add 7 address bytes each.
    LinkTimingProfile viaTwo = vhf1200();
    viaTwo.digiHops = 2;
    CHECK(ax25::frameAirtimeMs(viaTwo, 128) > vhfFrame,
          "digipeater hops lengthen the frame");

    // Degenerate input must not divide by zero.
    LinkTimingProfile broken;
    broken.baud = 0;
    CHECK(ax25::frameAirtimeMs(broken, 64) == 0, "zero baud yields zero airtime, not a crash");
}

static void testRecommendedT1()
{
    // The validation case: the model must reproduce, from first principles, the
    // empirically-tuned 6 s that has worked on 2 m for a year. If this drifts
    // far from ~4.6 s the model has stopped describing reality.
    const int vhfT1 = ax25::recommendedT1Ms(vhf1200(), 128);
    CHECK(vhfT1 > 4000 && vhfT1 < 6000,
          "VHF 1200 T1 lands near the empirically-good 6 s (~4.6 s)");

    // And the case that motivated all of this.
    const int hfT1 = ax25::recommendedT1Ms(hf300(), 64);
    CHECK(hfT1 > 11000 && hfT1 < 14000, "HF 300 paclen-64 T1 is ~12.6 s");

    // THE REGRESSION GUARD. The shipped default was a fixed 6000 ms. At 300
    // baud that is shorter than the time it takes to transmit one paclen-128
    // I-frame, so T1 expired before we had even stopped keying — every frame
    // retransmitted and every HF link died at N2. Any future change that lets a
    // profile's T1 fall below its own one-way frame time must fail here.
    CHECK(ax25::frameAirtimeMs(hf300(), 128) > 6000,
          "a paclen-128 HF frame takes longer to send than the old fixed 6 s T1");
    CHECK(ax25::recommendedT1Ms(hf300(), 128) > ax25::frameAirtimeMs(hf300(), 128),
          "T1 always exceeds the time to transmit the frame it is timing");
    CHECK(ax25::recommendedT1Ms(hf300(), 128) >= ax25::roundTripMs(hf300(), 128),
          "T1 always exceeds the modelled round trip");

    // Bigger frames need proportionally more time.
    CHECK(ax25::recommendedT1Ms(hf300(), 128) > ax25::recommendedT1Ms(hf300(), 64),
          "T1 grows with paclen");
    // And slower links need more than faster ones.
    CHECK(ax25::recommendedT1Ms(hf300(), 128) > ax25::recommendedT1Ms(vhf1200(), 128),
          "T1 grows as baud falls");
}

static void testDerivedTimersAndPaclen()
{
    CHECK(ax25::recommendedPaclen(300) == 64, "HF halves paclen — a 128-byte frame is 4 s of air");
    CHECK(ax25::recommendedPaclen(1200) == 128, "VHF keeps the efficient 128-byte frame");

    const int t1 = ax25::recommendedT1Ms(hf300(), 64);
    CHECK(ax25::recommendedT2Ms(t1) < t1,
          "T2 must be shorter than T1 or we retransmit before ever sending the ack");
    CHECK(ax25::recommendedT3Ms(t1) > t1,
          "T3 must be longer than T1 — polling while frames are in flight is pointless");
    CHECK(ax25::recommendedT3Ms(t1) >= ax25::kAx25MinT3Ms
              && ax25::recommendedT3Ms(t1) <= ax25::kAx25MaxT3Ms,
          "T3 stays inside its clamp");

    // A pathological profile must still yield usable timers.
    LinkTimingProfile slow = LinkTimingProfile::forBaud(45);
    CHECK(ax25::recommendedT1Ms(slow, 256) <= ax25::kAx25MaxT1Ms, "T1 is clamped at the top");
}

static void testProfileMatchesModulatorFraming()
{
    // The airtime model is only trustworthy if it reads the same preamble the
    // modulator actually transmits. forBaud() must pick the profile constants.
    CHECK(hf300().preambleFlags == ax25::kAx25Hf300PreambleFlags,
          "300 baud selects the HF preamble");
    CHECK(vhf1200().preambleFlags == ax25::kAx25Vhf1200PreambleFlags,
          "1200 baud selects the VHF preamble");
    CHECK(ax25::kAx25Hf300PreambleFlags > ax25::kAx25Vhf1200PreambleFlags,
          "HF uses the longer preamble");
}

// ---------------------------------------------------------------------------
// Link liveness
// ---------------------------------------------------------------------------

// A connected link with nothing outstanding must not sit "Connected" forever
// when the peer disappears: T3 polls it, and T1/N2 then declare it dead.
static void testIdlePollFiresOnSilentLink()
{
    Ax25Connection link;
    link.setLocalAddress(call("N0PMS-1"));
    link.setIdlePollMs(1000);

    QVector<Frame> sent;
    QObject::connect(&link, &Ax25Connection::sendFrame, [&](const QByteArray& raw) {
        if (auto f = Frame::decode(raw))
            sent.append(*f);
    });

    // Peer connects, then goes silent forever.
    link.onFrameReceived(Frame::makeU(call("N0PMS-1"), call("K7ABC"),
                                      FrameType::SABM, true, true));
    CHECK(link.isConnected(), "SABM brings the link up");
    CHECK(link.idlePollArmed(), "an idle connected link arms T3");

    sent.clear();
    const bool polled = spinUntil([&] { return link.stats().t3Polls > 0; }, 3000);
    CHECK(polled, "T3 fires on a link that has been silent");

    bool sawPoll = false;
    for (const Frame& f : sent) {
        if (f.type == FrameType::RR && f.pollFinal && f.command)
            sawPoll = true;
    }
    CHECK(sawPoll, "the idle check goes out as an RR command with P=1");
    CHECK(!link.idlePollArmed(), "T3 stands down once T1 is waiting on the poll");
}

// T3 is about idleness, not about being connected: while a frame is in flight
// T1 owns the link and T3 must stay out of the way.
static void testIdlePollDisarmedWhileBusy()
{
    Ax25Connection link;
    link.setLocalAddress(call("N0PMS-1"));
    link.setIdlePollMs(60000);
    QObject::connect(&link, &Ax25Connection::sendFrame, [](const QByteArray&) {});

    link.onFrameReceived(Frame::makeU(call("N0PMS-1"), call("K7ABC"),
                                      FrameType::SABM, true, true));
    CHECK(link.idlePollArmed(), "idle after connect");

    link.sendData(QByteArrayLiteral("hello\r"));
    CHECK(link.unacked() > 0, "the I-frame is outstanding");
    CHECK(!link.idlePollArmed(), "T3 disarms while an I-frame is in flight");

    // Acknowledge it; the link is idle again and T3 comes back.
    link.onFrameReceived(Frame::makeS(call("N0PMS-1"), call("K7ABC"),
                                      FrameType::RR, 1, true, false));
    CHECK(link.unacked() == 0, "the ack clears the window");
    CHECK(link.idlePollArmed(), "T3 re-arms once the link goes idle");
}

// A peer that REJs forever without ever accepting the retransmission used to
// reset our retry budget on every REJ: N2 was never reached and we retransmitted
// indefinitely, keying the radio on a link that would never recover.
static void testRejLoopIsBounded()
{
    Ax25Connection link;
    link.setLocalAddress(call("N0PMS-1"));
    link.setMaxRetries(4);

    int framesOut = 0;
    bool disconnected = false;
    QObject::connect(&link, &Ax25Connection::sendFrame, [&](const QByteArray&) { ++framesOut; });
    QObject::connect(&link, &Ax25Connection::disconnected,
                     [&](const Address&, bool) { disconnected = true; });

    link.onFrameReceived(Frame::makeU(call("N0PMS-1"), call("K7ABC"),
                                      FrameType::SABM, true, true));
    link.sendData(QByteArrayLiteral("data\r"));
    CHECK(link.unacked() == 1, "one I-frame in flight");

    // The peer rejects the same frame over and over, never acknowledging it.
    // N(R)=0 makes no progress, so the retry budget must not be replenished.
    for (int i = 0; i < 20 && !disconnected; ++i) {
        link.onFrameReceived(Frame::makeS(call("N0PMS-1"), call("K7ABC"),
                                          FrameType::REJ, 0, false, false));
    }

    CHECK(disconnected, "an endless REJ loop eventually fails the link instead of retransmitting forever");
    CHECK(link.stats().rejRecoveries <= 6,
          "REJ recovery is bounded by N2, not unbounded");
    CHECK(framesOut < 40, "we stop keying rather than retransmitting indefinitely");
}

// A peer repeating an acknowledgement that acknowledges nothing new proves
// nothing about progress and must not hold us below N2 forever.
static void testStaleAckDoesNotRefillRetryBudget()
{
    Ax25Connection link;
    link.setLocalAddress(call("N0PMS-1"));
    QObject::connect(&link, &Ax25Connection::sendFrame, [](const QByteArray&) {});

    link.onFrameReceived(Frame::makeU(call("N0PMS-1"), call("K7ABC"),
                                      FrameType::SABM, true, true));
    link.sendData(QByteArrayLiteral("data\r"));

    // An unpolled RR with N(R)=0 acknowledges nothing (V(A) is already 0).
    const int before = link.retries();
    for (int i = 0; i < 5; ++i) {
        link.onFrameReceived(Frame::makeS(call("N0PMS-1"), call("K7ABC"),
                                          FrameType::RR, 0, false, false));
    }
    CHECK(link.retries() == before, "a stale unpolled RR does not touch the retry counter");
    CHECK(link.unacked() == 1, "and does not acknowledge the outstanding frame");

    // A real acknowledgement does clear it.
    link.onFrameReceived(Frame::makeS(call("N0PMS-1"), call("K7ABC"),
                                      FrameType::RR, 1, false, false));
    CHECK(link.unacked() == 0, "N(R)=1 acknowledges the frame");
    CHECK(link.retries() == 0, "real progress clears the retry counter");
}

// AX.25 v2.2 callers open with SABME. Silence made them burn their whole retry
// budget before falling back to SABM; a prompt DM makes them fall back at once.
static void testSabmeRefusedWithDm()
{
    Ax25Connection link;
    link.setLocalAddress(call("N0PMS-1"));

    QVector<Frame> sent;
    QObject::connect(&link, &Ax25Connection::sendFrame, [&](const QByteArray& raw) {
        if (auto f = Frame::decode(raw))
            sent.append(*f);
    });

    Frame sabme = Frame::makeU(call("N0PMS-1"), call("K7ABC"), FrameType::SABME, true, true);
    // Round-trip it through the wire format so we also prove 0x6F decodes.
    auto decoded = Frame::decode(sabme.encode());
    CHECK(decoded.has_value(), "SABME encodes and decodes");
    CHECK(decoded && decoded->type == FrameType::SABME, "0x6F is recognised as SABME");

    link.onFrameReceived(*decoded);
    CHECK(sent.size() == 1, "SABME draws exactly one reply");
    CHECK(!sent.isEmpty() && sent.first().type == FrameType::DM,
          "we answer DM so the caller retries as v2.0 SABM instead of timing out");
    CHECK(!link.isConnected(), "we do not accept a mod-128 session");

    // ...and the v2.0 retry that follows is accepted normally.
    link.onFrameReceived(Frame::makeU(call("N0PMS-1"), call("K7ABC"),
                                      FrameType::SABM, true, true));
    CHECK(link.isConnected(), "the SABM fallback connects");
}

// The round-trip instrumentation is what makes a mis-sized T1 diagnosable at
// all, so its Karn exclusion has to actually hold.
static void testRttSamplingExcludesRetransmits()
{
    Ax25Connection link;
    link.setLocalAddress(call("N0PMS-1"));
    QObject::connect(&link, &Ax25Connection::sendFrame, [](const QByteArray&) {});

    link.onFrameReceived(Frame::makeU(call("N0PMS-1"), call("K7ABC"),
                                      FrameType::SABM, true, true));
    CHECK(link.stats().rttSamples == 0, "no samples before any exchange");

    link.sendData(QByteArrayLiteral("one\r"));
    link.onFrameReceived(Frame::makeS(call("N0PMS-1"), call("K7ABC"),
                                      FrameType::RR, 1, true, false));
    CHECK(link.stats().rttSamples == 1, "a cleanly acknowledged frame yields one RTT sample");

    // A frame that had to be REJ-retransmitted is ambiguous: its ack could be
    // answering either copy, so it must not be sampled.
    link.sendData(QByteArrayLiteral("two\r"));
    link.onFrameReceived(Frame::makeS(call("N0PMS-1"), call("K7ABC"),
                                      FrameType::REJ, 1, false, false));
    link.onFrameReceived(Frame::makeS(call("N0PMS-1"), call("K7ABC"),
                                      FrameType::RR, 2, true, false));
    CHECK(link.stats().rttSamples == 1,
          "a retransmitted frame contributes no RTT sample (Karn's algorithm)");
}

// BYE must be able to STOP a teardown, not restart it. disconnect() used to
// reset the retry budget on every call, so a second BYE handed the DISC
// retransmissions a fresh N2 — pressing it repeatedly kept the radio keying
// indefinitely. Found on the air 2026-07-31.
static void testSecondDisconnectDropsTheLink()
{
    Ax25Connection link;
    link.setLocalAddress(call("N0PMS-1"));

    QVector<Frame> sent;
    bool disconnected = false;
    QObject::connect(&link, &Ax25Connection::sendFrame, [&](const QByteArray& raw) {
        if (auto f = Frame::decode(raw))
            sent.append(*f);
    });
    QObject::connect(&link, &Ax25Connection::disconnected,
                     [&](const Address&, bool) { disconnected = true; });

    link.onFrameReceived(Frame::makeU(call("N0PMS-1"), call("K7ABC"),
                                      FrameType::SABM, true, true));
    CHECK(link.isConnected(), "connected");

    // First BYE: the polite teardown, one DISC on the air.
    sent.clear();
    link.disconnect();
    CHECK(link.state() == Ax25Connection::State::Disconnecting, "first disconnect starts teardown");
    CHECK(sent.size() == 1 && sent.first().type == FrameType::DISC, "first BYE sends DISC");

    // Second BYE while the peer has not answered: stop, do not re-arm.
    sent.clear();
    link.disconnect();
    CHECK(disconnected, "second BYE drops the link instead of retrying");
    CHECK(link.state() == Ax25Connection::State::Disconnected, "link is down");
    CHECK(sent.isEmpty(), "the forced drop transmits nothing — BYE must stop the radio, not key it");

    // And a third is harmless.
    link.disconnect();
    CHECK(sent.isEmpty(), "disconnecting an idle link is a no-op");
}

// reset() is the "the radio interface went away" path (the modem being switched
// off). It must tear the session down without putting anything on the air.
static void testResetDropsSilently()
{
    Ax25Connection link;
    link.setLocalAddress(call("N0PMS-1"));

    QVector<Frame> sent;
    QObject::connect(&link, &Ax25Connection::sendFrame, [&](const QByteArray& raw) {
        if (auto f = Frame::decode(raw))
            sent.append(*f);
    });

    link.onFrameReceived(Frame::makeU(call("N0PMS-1"), call("K7ABC"),
                                      FrameType::SABM, true, true));
    link.sendData(QByteArrayLiteral("data\r"));
    CHECK(link.unacked() == 1, "a frame is in flight");

    sent.clear();
    link.reset();
    CHECK(link.state() == Ax25Connection::State::Disconnected, "reset drops the link");
    CHECK(sent.isEmpty(), "reset transmits nothing");
    CHECK(!link.idlePollArmed(), "and leaves no timer armed");
}

// THE deadlock. A lost acknowledgement must not be able to kill a link on which
// every frame is being received perfectly. Reproduces the on-air failure of
// 2026-07-31: the far end decoded all nine retransmissions at conf ~1.0 and
// deliberately answered none of them, because N(S) != V(R) put a *duplicate*
// into the out-of-sequence branch, where the reject exception then silenced it.
static void testLostAckIsRecoveredNotDeadlocked()
{
    Ax25Connection link;
    link.setLocalAddress(call("N0PMS-1"));

    QVector<Frame> sent;
    QByteArray received;
    QObject::connect(&link, &Ax25Connection::sendFrame, [&](const QByteArray& raw) {
        if (auto f = Frame::decode(raw))
            sent.append(*f);
    });
    QObject::connect(&link, &Ax25Connection::dataReceived,
                     [&](const QByteArray& d) { received += d; });

    link.onFrameReceived(Frame::makeU(call("N0PMS-1"), call("K7ABC"),
                                      FrameType::SABM, true, true));

    // Peer's first I-frame, polled. We accept it and acknowledge.
    sent.clear();
    link.onFrameReceived(Frame::makeI(call("N0PMS-1"), call("K7ABC"), /*ns=*/0, /*nr=*/0,
                                      /*pf=*/true, QByteArrayLiteral("greeting")));
    CHECK(received == QByteArrayLiteral("greeting"), "first copy delivers its data");
    CHECK(link.recvSeq() == 1, "V(R) advanced");
    auto acks = [&] {
        int n = 0;
        for (const Frame& f : sent)
            if (f.type == FrameType::RR && f.nr == 1) ++n;
        return n;
    };
    CHECK(acks() == 1, "we acknowledge it");

    // That RR is lost on the air. The peer retransmits the SAME frame four
    // times, exactly as T1 tells it to. Every one must draw a fresh ack.
    for (int attempt = 0; attempt < 4; ++attempt) {
        sent.clear();
        link.onFrameReceived(Frame::makeI(call("N0PMS-1"), call("K7ABC"), /*ns=*/0, /*nr=*/0,
                                          /*pf=*/true, QByteArrayLiteral("greeting")));
        CHECK(acks() == 1,
              "a retransmitted frame is re-acknowledged, not answered with silence");
    }

    CHECK(received == QByteArrayLiteral("greeting"),
          "the duplicate payload is delivered exactly once");
    CHECK(link.stats().iDuplicate == 4, "duplicates are counted as such");
    CHECK(link.stats().iDropped == 0, "a duplicate is not a sequence gap");
    CHECK(link.isConnected(), "the link survives a lost acknowledgement");
}

// The reject exception must still hold for a REAL gap — that behaviour exists
// because answering every polled retransmit made a half-duplex link thrash.
static void testRealGapStillUsesRejectException()
{
    Ax25Connection link;
    link.setLocalAddress(call("N0PMS-1"));

    QVector<Frame> sent;
    QObject::connect(&link, &Ax25Connection::sendFrame, [&](const QByteArray& raw) {
        if (auto f = Frame::decode(raw))
            sent.append(*f);
    });

    link.onFrameReceived(Frame::makeU(call("N0PMS-1"), call("K7ABC"),
                                      FrameType::SABM, true, true));

    // V(R) is 0; the peer's frame 0 was lost and it sends 1, 2, 3 — a real gap.
    sent.clear();
    int rejCount = 0;
    for (int ns = 1; ns <= 3; ++ns) {
        link.onFrameReceived(Frame::makeI(call("N0PMS-1"), call("K7ABC"), ns, 0,
                                          /*pf=*/true, QByteArrayLiteral("x")));
    }
    for (const Frame& f : sent)
        if (f.type == FrameType::REJ) ++rejCount;
    CHECK(rejCount == 1, "exactly one REJ per gap, not one per frame");
    CHECK(link.stats().iDropped == 3, "gap frames are dropped");
    CHECK(link.stats().iDuplicate == 0, "and are not mistaken for duplicates");
}

// A duplicate whose N(R) frees our send window must not leave queued data
// stranded. The duplicate handler acknowledged the repeat and returned without
// pumping, so with nothing outstanding and T1 stopped there was nothing left to
// start the next segment: the mailbox sat silent with 516 bytes queued for the
// full 126 s until T3 shook it loose. Observed live 2026-07-31 — "it just
// stopped sending".
static void testDuplicateDoesNotStrandQueuedData()
{
    Ax25Connection link;
    link.setLocalAddress(call("N0PMS-1"));
    link.setPaclen(64);

    QVector<Frame> sent;
    QObject::connect(&link, &Ax25Connection::sendFrame, [&](const QByteArray& raw) {
        if (auto f = Frame::decode(raw))
            sent.append(*f);
    });

    link.onFrameReceived(Frame::makeU(call("N0PMS-1"), call("K7ABC"),
                                      FrameType::SABM, true, true));

    // A multi-segment reply: one frame goes out, the rest waits on the window.
    link.sendData(QByteArray(200, 'M'));
    CHECK(link.unacked() == 1, "window=1 puts one frame in flight");
    CHECK(link.sendQueueBytes() > 0, "the rest is queued");

    // The peer's I-frame arrives in sequence and is accepted.
    link.onFrameReceived(Frame::makeI(call("N0PMS-1"), call("K7ABC"), 0, 0,
                                      /*pf=*/true, QByteArrayLiteral("H\r")));

    // Now the SAME frame again — a duplicate, because its ack was lost — and
    // this time its N(R) acknowledges our outstanding segment.
    sent.clear();
    link.onFrameReceived(Frame::makeI(call("N0PMS-1"), call("K7ABC"), 0, /*nr=*/1,
                                      /*pf=*/true, QByteArrayLiteral("H\r")));

    CHECK(link.stats().iDuplicate >= 1, "the repeat is recognised as a duplicate");
    CHECK(link.unacked() == 1,
          "the freed window is immediately refilled from the queue, not left idle");
    bool sentData = false;
    for (const Frame& f : sent)
        if (f.type == FrameType::I) sentData = true;
    CHECK(sentData, "the next queued segment goes out on the duplicate's acknowledgement");
}

// The same guarantee, stated structurally: a connected link may never come to
// rest with the window free and data still queued.
static void testNeverIdleWithQueuedData()
{
    Ax25Connection link;
    link.setLocalAddress(call("N0PMS-1"));
    link.setPaclen(64);
    QObject::connect(&link, &Ax25Connection::sendFrame, [](const QByteArray&) {});

    link.onFrameReceived(Frame::makeU(call("N0PMS-1"), call("K7ABC"),
                                      FrameType::SABM, true, true));
    link.sendData(QByteArray(300, 'M')); // five segments at paclen 64

    // Walk the whole reply out, acknowledging one frame at a time the way a
    // half-duplex peer does. After every single exchange the invariant holds.
    for (int i = 0; i < 6 && link.isConnected(); ++i) {
        const bool queued = link.sendQueueBytes() > 0;
        if (queued) {
            CHECK(link.unacked() > 0,
                  "with data queued the link always has a frame in flight");
        }
        link.onFrameReceived(Frame::makeS(call("N0PMS-1"), call("K7ABC"),
                                          FrameType::RR, (i + 1) % 8, false, false));
    }
    CHECK(link.sendQueueBytes() == 0, "the whole reply drains");
}

// Pins the post-switch safety net specifically. It has to be reached through a
// branch that acknowledges but does NOT pump on its own, or the test passes with
// the net removed — the RR handler and the duplicate handler both pump, and the
// RNR handler cannot help because peer-busy blocks pumpOutbound() anyway.
//
// The out-of-sequence branch is the one: it calls ackUpTo() at the top of the
// I-frame case, then drops the frame and (at most once) sends a REJ, and
// deliberately returns without pumping. So a gap frame whose N(R) happens to
// acknowledge our outstanding segment leaves the window free with data queued,
// and only the net can restart us. Mutating the net to `if (false)` fails here.
static void testSafetyNetCoversBranchesThatDoNotPump()
{
    Ax25Connection link;
    link.setLocalAddress(call("N0PMS-1"));
    link.setPaclen(64);

    QVector<Frame> sent;
    QObject::connect(&link, &Ax25Connection::sendFrame, [&](const QByteArray& raw) {
        if (auto f = Frame::decode(raw))
            sent.append(*f);
    });

    link.onFrameReceived(Frame::makeU(call("N0PMS-1"), call("K7ABC"),
                                      FrameType::SABM, true, true));
    link.sendData(QByteArray(200, 'M')); // four segments at paclen 64
    CHECK(link.unacked() == 1, "one segment in flight");
    CHECK(link.sendQueueBytes() > 0, "more waiting");

    // A genuine gap — V(R) is 0 and the peer sends N(S)=2 — carrying N(R)=1,
    // which acknowledges the segment we have outstanding.
    sent.clear();
    link.onFrameReceived(Frame::makeI(call("N0PMS-1"), call("K7ABC"), /*ns=*/2, /*nr=*/1,
                                      /*pf=*/true, QByteArrayLiteral("gap")));
    CHECK(link.stats().iDropped == 1, "the gap frame is dropped, not accepted");
    CHECK(link.stats().iDuplicate == 0, "and is not mistaken for a duplicate");

    CHECK(link.unacked() == 1,
          "the acknowledged window is refilled — the link must not come to rest "
          "with data queued just because the branch that freed it does not pump");
    bool sentData = false;
    for (const Frame& f : sent)
        if (f.type == FrameType::I) sentData = true;
    CHECK(sentData, "the next segment actually goes on the air");
}

// Pins "T1 re-arms only on a real acknowledgement". Mutating the guard back to
// an unconditional startT1() must fail here: a stream of stale RRs that
// acknowledge nothing would keep pushing the deadline out, so T1 would never
// expire and no retransmission would ever happen.
static void testStaleAcksDoNotPostponeT1()
{
    Ax25Connection link;
    link.setLocalAddress(call("N0PMS-1"));
    link.setRetryTimeoutMs(1000); // keep the test short
    QObject::connect(&link, &Ax25Connection::sendFrame, [](const QByteArray&) {});

    link.onFrameReceived(Frame::makeU(call("N0PMS-1"), call("K7ABC"),
                                      FrameType::SABM, true, true));
    link.sendData(QByteArrayLiteral("data\r"));
    CHECK(link.unacked() == 1, "a frame is outstanding");

    // Feed a stale, unpolled RR every ~250 ms for 2 s — faster than the 1000 ms
    // T1, so anything that re-arms the timer keeps it permanently ahead of the
    // deadline. N(R)=0 acknowledges nothing (V(A) is already 0), so none of
    // these is progress.
    //
    // The assertion has to land INSIDE the feeding window: wait until after it
    // and T1 fires 1000 ms past the last stale ack either way, which is exactly
    // why the first version of this test passed with the guard mutated away.
    QDeadlineTimer feeding(2000);
    while (!feeding.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        link.onFrameReceived(Frame::makeS(call("N0PMS-1"), call("K7ABC"),
                                          FrameType::RR, 0, false, false));
    }

    CHECK(link.stats().t1Timeouts > 0,
          "T1 expires on schedule while stale acknowledgements keep arriving — a "
          "frame that acknowledges nothing must not postpone the deadline");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testFrameAirtime();
    testRecommendedT1();
    testDerivedTimersAndPaclen();
    testProfileMatchesModulatorFraming();

    testIdlePollFiresOnSilentLink();
    testIdlePollDisarmedWhileBusy();
    testRejLoopIsBounded();
    testStaleAckDoesNotRefillRetryBudget();
    testSabmeRefusedWithDm();
    testRttSamplingExcludesRetransmits();
    testSecondDisconnectDropsTheLink();
    testResetDropsSilently();
    testLostAckIsRecoveredNotDeadlocked();
    testRealGapStillUsesRejectException();
    testDuplicateDoesNotStrandQueuedData();
    testNeverIdleWithQueuedData();
    testSafetyNetCoversBranchesThatDoNotPump();
    testStaleAcksDoNotPostponeT1();

    if (g_failures == 0)
        std::fprintf(stderr, "ax25_link_timing_test: all checks passed\n");
    else
        std::fprintf(stderr, "ax25_link_timing_test: %d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
