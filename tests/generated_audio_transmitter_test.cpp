// Shared generated-audio transmitter (RFC #4214): the key → send → drain →
// unkey sequence against a scripted route — Flex (stream first), HL2 (no
// stream, command-edge PTT) and Icom (radio PTT readback) answers, plus every
// fail-closed path. No radio, no audio device.
// Run: ./build/generated_audio_transmitter_test

#include "FakeTxAudioRoute.h"
#include "core/GeneratedAudioTransmitter.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

#include <cstdio>
#include <cstring>
#include <string>

using namespace AetherSDR;
using Outcome = GeneratedAudioTransmitter::Outcome;
using Phase = GeneratedAudioTransmitter::Phase;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-60s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

GeneratedAudioTiming fastTiming()
{
    GeneratedAudioTiming t;
    t.streamWaitMs = 150;
    t.settleMs = 5;
    t.pttConfirmMs = 150;
    t.leadMs = 5;
    t.chunkMs = 5;
    t.leadBufferMs = 20;
    t.finishWaitMs = 200;
    t.tailMs = 10;
    return t;
}

// ms of 24 kHz stereo float32, a ramp so order and content are checkable.
QByteArray audio(int ms)
{
    const int frames = 24 * ms;
    QByteArray pcm(frames * GeneratedAudioTransmitter::kFrameBytes, Qt::Uninitialized);
    auto* f = reinterpret_cast<float*>(pcm.data());
    for (int i = 0; i < frames; ++i)
        f[2 * i] = f[2 * i + 1] = float(i % 1000) / 1000.0f;
    return pcm;
}

struct Result {
    bool done = false;
    Outcome outcome = Outcome::Completed;
    QString reason;
    qint64 atMs = -1;
};

struct Harness {
    FakeTxAudioRoute route;
    GeneratedAudioTransmitter tx;
    Result result;

    explicit Harness(GeneratedAudioTiming timing = fastTiming())
        : tx(&route, nullptr, timing)
    {
        route.tx = &tx;
        QObject::connect(&tx, &GeneratedAudioTransmitter::finished,
                         [this](Outcome o, const QString& r) {
                             result = {true, o, r, route.clock.elapsed()};
                         });
    }

    // Spin the event loop until finished or ms pass; true if finished.
    bool waitDone(int ms = 2000)
    {
        QElapsedTimer t;
        t.start();
        while (!result.done && t.elapsed() < ms)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        return result.done;
    }
    void spin(int ms)
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
    }
    std::string trace() const { return route.calls.join(",").toStdString(); }
};

bool ordered(const QStringList& calls, const QStringList& expected)
{
    int from = 0;
    for (const QString& c : expected) {
        const int i = calls.indexOf(c, from);
        if (i < 0) return false;
        from = i + 1;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // Flex: no stream yet → request it, touch nothing else until it arrives,
    // then claim → key → send every byte → barrier → drain + tail → unkey.
    {
        Harness h;
        h.route.needs = true;
        h.route.drainMs = 40;
        const QByteArray pcm = audio(100);
        QString err;
        const bool started = h.tx.start(pcm, err);
        h.spin(30);
        // The request is admitted before the wait, so it is held across it; the
        // path is still unclaimed and the radio unkeyed.
        const bool waited = h.tx.phase() == Phase::WaitingForStream
                            && h.route.calls == QStringList{"requestStream", "admit"};
        h.route.ready = true;
        h.tx.onStreamReady();
        const bool done = h.waitDone();
        const qint64 finishAt = h.route.timeOf("finish");
        const qint64 offAt = h.route.timeOf("keyOff");
        report("flex_waits_for_stream_then_sends_all_and_unkeys_after_drain",
               started && waited && done && h.result.outcome == Outcome::Completed
                   && ordered(h.route.calls, {"requestStream", "admit", "claim", "keyOn",
                                              "send", "finish", "keyOff", "release", "clear"})
                   && h.route.sent == pcm && offAt - finishAt >= 40 + 10 && !h.tx.isActive(),
               h.trace() + " drainHold=" + std::to_string(offAt - finishAt));
    }

    // The stream never arrives: fail closed without keying or claiming.
    {
        Harness h;
        h.route.needs = true;
        QString err;
        h.tx.start(audio(50), err);
        const bool done = h.waitDone();
        report("stream_that_never_arrives_fails_without_keying",
               done && h.result.outcome == Outcome::Failed
                   && h.result.reason.contains("did not arrive")
                   && !h.route.calls.contains("keyOn") && !h.route.calls.contains("claim"),
               h.trace());
    }

    // A refused stream request, and a route refusal, stop start() itself.
    {
        Harness h;
        h.route.needs = true;
        h.route.requestAccepted = false;
        QString err1;
        const bool s1 = h.tx.start(audio(50), err1);
        h.route.requestAccepted = true;
        h.route.refusal = QStringLiteral("Not a voice mode.");
        QString err2;
        const bool s2 = h.tx.start(audio(50), err2);
        QString err3;
        const bool s3 = h.tx.start(QByteArray(), err3);
        h.spin(20);
        report("refusals_return_false_with_reason_and_do_nothing",
               !s1 && !err1.isEmpty() && !s2 && err2 == QLatin1String("Not a voice mode.")
                   && !s3 && !err3.isEmpty() && !h.result.done && !h.tx.isActive()
                   && h.route.calls == QStringList{"requestStream"},
               h.trace());
    }

    // HL2-shaped route: no stream, command-edge PTT.
    {
        Harness h;
        const QByteArray pcm = audio(60);
        QString err;
        h.tx.start(pcm, err);
        const bool done = h.waitDone();
        report("seam_route_needs_no_stream_and_completes",
               done && h.result.outcome == Outcome::Completed && h.route.sent == pcm
                   && !h.route.calls.contains("requestStream")
                   && ordered(h.route.calls, {"claim", "keyOn", "send", "finish", "keyOff", "release"}),
               h.trace());
    }

    // Icom-shaped route: sample zero waits for the radio's own PTT readback.
    {
        Harness h;
        h.route.waitsRadio = true;
        const QByteArray pcm = audio(60);
        QString err;
        h.tx.start(pcm, err);
        h.spin(60);
        const bool heldBack = h.route.calls.contains("keyOn") && h.route.sendCalls == 0
                              && h.tx.phase() == Phase::WaitingForPtt;
        h.tx.onRadioKeyed(false);   // not a confirmation
        h.spin(20);
        const bool stillHeld = h.route.sendCalls == 0;
        h.route.radioKeyed = true;
        h.tx.onRadioKeyed(true);
        const bool done = h.waitDone();
        report("radio_ptt_readback_route_holds_audio_until_confirmed",
               heldBack && stillHeld && done && h.result.outcome == Outcome::Completed
                   && h.route.sent == pcm,
               h.trace());
    }
    {
        Harness h;
        h.route.waitsRadio = true;
        QString err;
        h.tx.start(audio(60), err);
        const bool done = h.waitDone();
        report("unconfirmed_radio_ptt_fails_closed_and_unkeys",
               done && h.result.outcome == Outcome::Failed
                   && h.result.reason.contains("did not confirm") && h.route.sendCalls == 0
                   && ordered(h.route.calls, {"claim", "keyOn", "keyOff", "release", "clear"}),
               h.trace());
    }

    // PTT refused synchronously (an interlock): no audio, and the request taken
    // at the boundary is handed back even though the radio never showed keyed.
    {
        Harness h;
        h.route.keyEngages = false;
        h.route.onKeyOn = [&] { h.tx.onPttBlocked(QStringLiteral("TX inhibited")); };
        QString err;
        h.tx.start(audio(60), err);
        const bool done = h.waitDone();
        report("ptt_blocked_aborts_before_any_audio",
               done && h.result.outcome == Outcome::Failed && h.result.reason == "TX inhibited"
                   && h.route.sendCalls == 0 && h.route.calls.contains("keyOff")
                   && h.route.calls.contains("release"),
               h.trace());
    }
    {
        Harness h;
        h.route.keyEngages = false;
        QString err;
        h.tx.start(audio(60), err);
        const bool done = h.waitDone();
        report("ptt_that_does_not_engage_aborts",
               done && h.result.outcome == Outcome::Failed
                   && h.result.reason.contains("did not engage") && h.route.sendCalls == 0
                   // The key never engaged, so the old release test (isKeyed())
                   // skipped keyOff and the request stayed admitted.
                   && h.route.calls.contains("keyOff"),
               h.trace());
    }

    // No transmit authority at all: the route cannot even REQUEST the key
    // (#5659 — no admitted operation, so nothing it sent would be licensed to
    // reach the transport). Keying anyway would put a silent carrier on the
    // air, which is the failure the operator cannot hear.
    {
        Harness h;
        h.route.keyAccepted = false;
        QString err;
        const bool started = h.tx.start(audio(60), err);
        const bool done = h.waitDone();
        report("refused_key_request_fails_closed_without_keying_or_audio",
               started && done && h.result.outcome == Outcome::Failed
                   && h.result.reason.contains("refused")
                   && h.route.sendCalls == 0 && !h.route.keyed
                   && !h.route.calls.contains("send")
                   && h.route.calls.contains("release"),
               h.trace());
    }

    // A PTT refusal for someone else (a TUNE) mid-message is not ours.
    {
        Harness h;
        const QByteArray pcm = audio(200);
        QString err;
        h.tx.start(pcm, err);
        h.spin(60);
        h.tx.onPttBlocked(QStringLiteral("TUNE not started"));
        const bool done = h.waitDone();
        report("unrelated_ptt_refusal_mid_message_is_ignored",
               done && h.result.outcome == Outcome::Completed && h.route.sent == pcm, h.trace());
    }

    // Audio is paced to real time, not dumped: sending 400 ms must take a
    // good part of 400 ms (the cushion is 20 ms here).
    {
        Harness h;
        QString err;
        h.tx.start(audio(400), err);
        const bool done = h.waitDone();
        const qint64 sendSpan = h.route.timeOf("finish") - h.route.firstSendMs;
        report("audio_is_paced_to_real_time",
               done && h.result.outcome == Outcome::Completed && sendSpan >= 300
                   && h.route.sendCalls > 10,
               "span=" + std::to_string(sendSpan) + " calls=" + std::to_string(h.route.sendCalls));
    }

    // Operator stop mid-message: unkey at once, no barrier, no tail.
    {
        Harness h;
        const QByteArray pcm = audio(500);
        QString err;
        h.tx.start(pcm, err);
        h.spin(80);
        const bool sending = h.tx.phase() == Phase::Sending;
        h.tx.stop();
        report("operator_stop_unkeys_immediately",
               sending && h.result.done && h.result.outcome == Outcome::Stopped
                   && h.route.sent.size() < pcm.size() && !h.route.calls.contains("finish")
                   && ordered(h.route.calls, {"keyOn", "send", "keyOff", "release", "clear"})
                   && !h.route.keyed,
               h.trace());
        // Nothing left over fires later.
        const int callsAfter = h.route.calls.size();
        h.spin(100);
        report("nothing_fires_after_stop", h.route.calls.size() == callsAfter, h.trace());
    }

    // Key released under us (MOX off): stop; but not before we asked to key.
    {
        Harness h(fastTiming());
        GeneratedAudioTiming slow = fastTiming();
        slow.settleMs = 40;
        Harness hs(slow);
        QString err;
        hs.tx.start(audio(100), err);
        hs.tx.onKeyReleased();   // during Settling: someone else's release
        const bool ignored = hs.tx.isActive();
        const bool completed = hs.waitDone() && hs.result.outcome == Outcome::Completed;

        h.tx.start(audio(500), err);
        h.spin(80);
        h.route.keyed = false;   // the operator released MOX
        h.tx.onKeyReleased();
        report("key_released_under_us_stops_but_only_once_keyed",
               ignored && completed && h.result.done && h.result.outcome == Outcome::Stopped
                   && h.route.calls.contains("release"),
               hs.trace() + " | " + h.trace());
    }

    // The end-of-audio barrier never comes back: unkey anyway.
    {
        Harness h;
        h.route.drainMs = -1;
        QString err;
        h.tx.start(audio(40), err);
        const bool done = h.waitDone();
        report("lost_end_of_audio_barrier_fails_closed",
               done && h.result.outcome == Outcome::Failed
                   && ordered(h.route.calls, {"finish", "keyOff", "release"}),
               h.trace());
    }
    {
        Harness h;
        h.route.finishQueued = false;
        QString err;
        h.tx.start(audio(40), err);
        const bool done = h.waitDone();
        report("unqueueable_barrier_fails_closed",
               done && h.result.outcome == Outcome::Failed && h.route.calls.contains("keyOff"),
               h.trace());
    }

    // A barrier carrying another transmission's token is not ours.
    {
        Harness h;
        h.route.drainMs = -1;
        QString err;
        h.tx.start(audio(40), err);
        QElapsedTimer t;
        t.start();
        while (h.tx.phase() != Phase::Draining && t.elapsed() < 1000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        h.tx.onAudioFinished(0xDEAD, 0);
        h.spin(40);
        const bool stillDraining = h.tx.phase() == Phase::Draining;
        report("stale_barrier_token_is_ignored", stillDraining && !h.result.done, h.trace());
        h.tx.stop();
    }

    // Busy: a second start while one runs is refused, the first unharmed.
    {
        Harness h;
        const QByteArray pcm = audio(80);
        QString err1, err2;
        h.tx.start(pcm, err1);
        const bool second = h.tx.start(audio(80), err2);
        const bool done = h.waitDone();
        report("second_start_while_active_is_refused",
               !second && err2.contains("already") && done
                   && h.result.outcome == Outcome::Completed && h.route.sent == pcm,
               h.trace());
    }

    // A timer left over from a stopped transmission must not key the next one
    // early.
    {
        GeneratedAudioTiming t = fastTiming();
        t.settleMs = 60;
        Harness h(t);
        QString err;
        h.tx.start(audio(40), err);
        h.spin(30);
        h.tx.stop();
        h.result = {};
        h.route.calls.clear();
        h.route.at.clear();
        const qint64 restartAt = h.route.clock.elapsed();
        h.tx.start(audio(40), err);
        const bool done = h.waitDone();
        const qint64 keyedAfter = h.route.timeOf("keyOn") - restartAt;
        report("stale_timer_does_not_key_the_next_transmission_early",
               done && keyedAfter >= 55, "keyedAfter=" + std::to_string(keyedAfter));
    }

    std::printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
