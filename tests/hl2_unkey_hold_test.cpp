// #5497 — the unkey mute is released AFTER the radio stops transmitting, not
// before it. Stated as an ORDERING, because that is what the defect is.
//
// THE DEFECT, precisely. Hl2Backend::applyKeying() queued the demodulator's
// unmute and, sixty-odd lines later in the same function, queued the MOX-off.
// Both targets live on the SAME thread — Hl2RxDsp and MetisClient are both
// moved to Hl2Backend's I/O thread — and both edges ride Qt::QueuedConnection,
// so delivery is FIFO in posting order. The unmute was not racing the MOX-off
// and did not sometimes lose: it was guaranteed to be delivered first. The
// demodulator listened at full gain while the PA was still up, for the whole
// T/R turnaround, on every unkey. Measured on ON8ST's own HL2 into a dummy
// load, that is a burst 57.5 dB above the receiver's own resting floor which
// leaves the float domain at +10.6 dBFS.
//
// WHAT THIS FILE ASSERTS, and why in this shape:
//
//   1. THE ORDERING, not the delay. After key-up plus one full turn of the I/O
//      thread's event loop the MOX-off has been APPLIED and the demodulator is
//      STILL MUTED. On stock the same turn delivers both, so the demodulator is
//      already listening — the test fails there, which is the point.
//
//   2. THE POSITIVE CONTROL, and it is not optional here. "Still muted after
//      one turn" is also what a dropped invokeMethod, a renamed slot, a DSP
//      that was never attached and a test that forgot to pump its event loop
//      all look like. So the same sequence is run with the hold set to ZERO and
//      the release is required to be IMMEDIATE. That is the one arrangement in
//      which "still muted at 0 ms" can only mean the hold — every other
//      explanation above would keep it muted at zero too.
//
//   3. THE RE-KEY GUARD. Key up, wait half the hold, key down again, wait past
//      the ORIGINAL expiry: still muted. A hold that fired on its own schedule
//      would unmute in the middle of the second transmission, which is the
//      failure the hold exists to prevent, arriving by a different door.
//
//   4. THE MIXER GATES ON THE SAME FLAG. mixReceiverAudio()'s early return used
//      to read `m_keyed && !m_txMonitor` while the demodulator was muted on an
//      expression evaluated elsewhere. Deferring one and not the other would
//      have pushed the whole hold into the engine as digital ZEROS; gating both
//      on m_rxAudioMuted makes it a continued GAP instead.
//
//   5. THE HOLD IS ARMED BY AN EDGE, NOT BY A STATE. A second setKeying(false)
//      arriving with the backend ALREADY unkeyed started no T/R turnaround, so
//      it has none of its own to cover. It used to restart the single-shot
//      timer from zero, which put the mute up to TWICE the hold past the real
//      MOX-off -- and this change's own accounting charges every one of those
//      milliseconds to #5498. This leg MEASURES the mute, so the number in the
//      report is read off the run rather than reasoned about.
//
//   6. THE MONITOR IS A DIAGNOSTIC AND `on` HAS TWO VALUES. Turning the
//      monitor ON inside an armed hold asks to HEAR the receiver, and must be
//      answered now; turning it OFF inside one asks for nothing, and must not
//      cancel the hold. Both directions are asserted, because the branch that
//      protects the second used to swallow the first as well.
//
//   7. CW FULL BREAK-IN SKIPS THE HOLD, AND SEMI-BREAK-IN DOES NOT. Ruled by
//      ON8ST on #5850: an operator who turns QSK on has asked to hear between
//      elements and accepts the leak. Asserted on the TIMER — "no hold is
//      armed" — rather than on the mute, because an expired hold also leaves a
//      chain unmuted. Both sides are driven: full break-in through
//      setCwKeying(breakIn=true), semi-break-in as elements inside an MOX the
//      operator asserted, whose release still gets the whole hold.
//
// WHAT IT DOES NOT ASSERT, said here rather than left to be discovered: any
// audio level, anything about the HL2's actual T/R turnaround, whether 70 ms is
// the right number, and — new with leg 7 — the ~17 WPM crossover. That figure
// is ARITHMETIC from two constants in Hl2Backend (1200/WPM against the 70 ms
// hold, or against the 76 ms the 6 ms envelope hang plus that hold actually
// costs). NO CW MEASUREMENT EXISTS anywhere behind this change: #5497's eleven
// windows are all 4-second SSB keys. These legs assert armed-or-not-armed,
// which is a fact about the code, and leave the speed to arithmetic.
// Those are bench questions and #5497 carries the measurements. This is an
// ordering test with a clock in it.
//
// THE CLOCK, AND WHERE IT IS WALL-CLOCK. Legs 1, 2, 4 and the monitor
// directions assert on state at a named instant and hold no stopwatch. The
// re-key leg, the redundant-unkey leg and the monitor-off control DO run on
// wall-clock time: they pump a real event loop and read a real QTimer, so they
// carry generous margins (a hold of 200-400 ms where the shipped value is 70,
// and a 10x ceiling on every deadline) rather than tight ones. If one of them
// ever flakes, that is where to look first -- the margins, not the logic.
//
// ONE GUARD IS DELIBERATELY UNCOVERED. The hold timer's callback opens with
// `if (m_keyed && !m_txMonitor) return;`. A re-key inside the window stops the
// timer, so that callback is NEVER EXPECTED TO FIRE while keyed and no leg
// here provokes it. It is belt and braces with the stop in applyKeying(), kept
// because "should be impossible" is how a hold turns into an unmute in the
// middle of a transmission. Nobody should go hunting for its coverage.
//
// NO WDSP, NO SOCKET, NO RADIO. The probe Hl2RxDsp is never configure()d, so it
// owns no WDSP channel; setAudioMuted() on an unconfigured chain is a plain
// member write plus a null-checked noise-blanker hold. MetisClient is never
// start()ed — the transmit gate is opened directly, exactly as
// hl2_tx_gate_test.cpp does it, and MOX is read back from the client's own
// applied state on the thread that owns it.

#include "TestSettingsProfile.h"
#include "TxTestAuthority.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisClient.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <cstdio>

namespace AetherSDR::hl2 {

struct Hl2UnkeyHoldTestAccess {
    // Open the transmit gate without a socket, a peer or a discovery cycle —
    // the same preparation hl2_tx_gate_test.cpp uses.
    static void prepare(Hl2Backend& backend)
    {
        backend.m_txAllowed = true;
        QMetaObject::invokeMethod(backend.m_metis, [metis = backend.m_metis] {
            metis->enableTransmit(true);
        }, Qt::BlockingQueuedConnection);
    }

    // Give receiver 0 a real Hl2RxDsp, on the real I/O thread.
    //
    // ON THE I/O THREAD IS THE WHOLE POINT. The defect is that the unmute and
    // the MOX-off share one event queue in posting order; a probe parked on the
    // main thread would sit in a different queue and could not observe the
    // ordering the bug lives in. Unconfigured, so no WDSP channel exists.
    static void attachProbeDsp(Hl2Backend& backend)
    {
        auto* dsp = new Hl2RxDsp();
        dsp->moveToThread(backend.m_ioThread);
        backend.m_rx[0].dsp = dsp;
        backend.publishIoDsps();
    }

    // ONE FULL TURN of the I/O thread's event loop, and the MOX state as seen
    // from the thread that owns it.
    //
    // A blocking invoke posted after the key edge returns only once everything
    // posted BEFORE it has been handled, because the queue is FIFO — so when
    // this returns, any unmute or MOX-off that applyKeying() posted has been
    // delivered. That is what makes "still muted" a statement about the hold
    // rather than about timing luck. Reading MetisClient::isKeyed() inside the
    // lambda keeps the read on the owning thread instead of racing it.
    static bool moxOffApplied(Hl2Backend& backend)
    {
        bool keyed = true;
        QMetaObject::invokeMethod(backend.m_metis, [&keyed, metis = backend.m_metis] {
            keyed = metis->isKeyed();
        }, Qt::BlockingQueuedConnection);
        return !keyed;
    }

    // The DSP's OWN applied state, read on the thread that writes it. Not the
    // backend's request — see Hl2RxDsp::isAudioMuted().
    static bool dspMuted(Hl2Backend& backend)
    {
        bool muted = false;
        Hl2RxDsp* dsp = backend.m_rx[0].dsp;
        QMetaObject::invokeMethod(dsp, [&muted, dsp] {
            muted = dsp->isAudioMuted();
        }, Qt::BlockingQueuedConnection);
        return muted;
    }

    // The mixer's gate, which since #5497 is the same flag the demodulator is
    // muted on. Read on the backend's own thread, which is this one.
    static bool mixerGateClosed(const Hl2Backend& backend) { return backend.m_rxAudioMuted; }

    static void setHoldMs(Hl2Backend& backend, int ms) { backend.m_unkeyUnmuteHoldMs = ms; }
    static int holdMs(const Hl2Backend& backend) { return backend.m_unkeyUnmuteHoldMs; }
    static int defaultHoldMs() { return Hl2Backend::kUnkeyUnmuteHoldMs; }

    // IS A HOLD ARMED, structurally — not "is the chain still muted", which a
    // stuck mute would also answer yes to. This is the claim the CW ruling
    // makes: in full break-in no hold is ARMED at all, which is a stronger and
    // clock-free statement than "it unmuted quickly".
    static bool holdArmed(const Hl2Backend& backend)
    {
        return backend.m_unkeyUnmuteTimer && backend.m_unkeyUnmuteTimer->isActive();
    }

    // setCwKeying() refuses a key-down outside CW mode, and rightly: a key
    // binding pressed in SSB must not become an unmodulated carrier. The probe
    // receiver defaults to USB, so the CW legs have to say what mode the
    // operator is in. Written directly because the public path to it wants a
    // configured WDSP chain, and this file deliberately has none.
    static void setTxReceiverMode(Hl2Backend& backend, const char* mode)
    {
        backend.m_rx[static_cast<std::size_t>(backend.m_txDdc)].mode =
            QString::fromLatin1(mode);
    }

    static void tearDown(Hl2Backend& backend) { backend.tearDownReceivers(); }
};

}  // namespace AetherSDR::hl2

using AetherSDR::hl2::Hl2Backend;
using AetherSDR::hl2::Hl2UnkeyHoldTestAccess;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

// Spin the BACKEND's thread (this one) until the hold has had its chance.
//
// The hold is a QTimer owned by the backend, so it fires on this thread and
// only while this thread is in its event loop. `deadlineMs` is a generous
// ceiling, not the quantity under test: every assertion below is about the
// state at a named instant, and this helper only guarantees that instant has
// been reached. Returns as soon as the DSP unmutes so a passing run is quick.
static void pumpUntilUnmutedOr(Hl2Backend& backend, int deadlineMs)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < deadlineMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (!Hl2UnkeyHoldTestAccess::dspMuted(backend))
            return;
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

// Spin for a fixed wall-clock interval WITHOUT stopping early, for the cases
// that must assert "still muted after this much time".
static void pumpFor(int ms)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}

int main(int argc, char** argv)
{
    // Before QCoreApplication, as the header requires: Hl2Backend reads the
    // settings store on construction and must not touch the real one.
    TestSettingsProfile profile(QStringLiteral("hl2-unkey-hold-test"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "settings are isolated");
    std::printf("\n  #5497 — the unkey unmute waits for the radio's T/R\n\n");

    // ---- 1. THE ORDERING -----------------------------------------------
    //
    // This is the case that fails on stock. Nothing about it depends on the
    // hold's VALUE beyond it being long enough to still be running when the
    // assertion is made, which is why the assertion is made immediately after
    // one event-loop turn rather than after a sleep.
    {
        TxTestAuthority tx;
        Hl2Backend backend;
        Hl2UnkeyHoldTestAccess::prepare(backend);
        Hl2UnkeyHoldTestAccess::attachProbeDsp(backend);
        check(Hl2UnkeyHoldTestAccess::holdMs(backend)
                  == Hl2UnkeyHoldTestAccess::defaultHoldMs(),
              "the hold is a member seeded from the named constant, not a literal");

        check(!Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "an idle receiver is not muted");
        check(!Hl2UnkeyHoldTestAccess::mixerGateClosed(backend),
              "an idle mixer gate is open");

        // KEY DOWN. The mute is immediate on this edge and deliberately so:
        // the operator cannot want to hear a transmitter that has not started.
        backend.setKeying(true, tx.operation);
        check(Hl2UnkeyHoldTestAccess::mixerGateClosed(backend),
              "key-down closes the mixer gate synchronously");
        check(Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "key-down mutes the demodulator within one event-loop turn");

        // KEY UP, then exactly one turn of the I/O thread's event loop.
        backend.setKeying(false, tx.operation);
        const bool moxOff = Hl2UnkeyHoldTestAccess::moxOffApplied(backend);
        const bool stillMuted = Hl2UnkeyHoldTestAccess::dspMuted(backend);
        check(moxOff,
              "key-up applies the MOX-off within one event-loop turn");
        // THE INVARIANT. On stock this is false, because the unmute was posted
        // ahead of the MOX-off and the same turn delivered it.
        check(stillMuted,
              "#5497: the demodulator is STILL MUTED after the MOX-off has been applied");
        check(moxOff && stillMuted,
              "#5497: the MOX-off is queued BEFORE the unmute lands, not after");
        check(Hl2UnkeyHoldTestAccess::mixerGateClosed(backend),
              "the mixer gates on the same flag, so the hold is a GAP and not zeros");

        // And it does come back — a hold that never released would pass every
        // assertion above and silence the radio.
        pumpUntilUnmutedOr(backend, 10 * Hl2UnkeyHoldTestAccess::defaultHoldMs() + 500);
        check(!Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "the hold expires and the demodulator unmutes");
        check(!Hl2UnkeyHoldTestAccess::mixerGateClosed(backend),
              "the mixer gate reopens with it");

        Hl2UnkeyHoldTestAccess::tearDown(backend);
    }

    // ---- 2. THE POSITIVE CONTROL ---------------------------------------
    //
    // Same sequence, hold set to ZERO. "Still muted after one turn" must now be
    // FALSE. A dropped invokeMethod, a renamed slot, an unattached DSP or a
    // forgotten processEvents() would all leave it TRUE here, so this is what
    // separates a working hold from a broken pipe.
    {
        TxTestAuthority tx;
        Hl2Backend backend;
        Hl2UnkeyHoldTestAccess::prepare(backend);
        Hl2UnkeyHoldTestAccess::attachProbeDsp(backend);
        Hl2UnkeyHoldTestAccess::setHoldMs(backend, 0);

        backend.setKeying(true, tx.operation);
        check(Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "control: key-down still mutes with the hold at zero");

        backend.setKeying(false, tx.operation);
        check(Hl2UnkeyHoldTestAccess::moxOffApplied(backend),
              "control: the MOX-off is applied with the hold at zero");
        check(!Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "POSITIVE CONTROL: at a hold of ZERO the release is IMMEDIATE — "
              "so 'still muted' above can only be the hold");
        check(!Hl2UnkeyHoldTestAccess::mixerGateClosed(backend),
              "control: the mixer gate reopens immediately at a hold of zero");

        Hl2UnkeyHoldTestAccess::tearDown(backend);
    }

    // ---- 3. THE RE-KEY GUARD -------------------------------------------
    //
    // Key up, wait half the hold, key down again, then wait past the ORIGINAL
    // expiry. A timer left running would unmute the receiver in the middle of
    // the second transmission — the original defect, arriving late instead of
    // early.
    {
        TxTestAuthority tx;
        Hl2Backend backend;
        Hl2UnkeyHoldTestAccess::prepare(backend);
        Hl2UnkeyHoldTestAccess::attachProbeDsp(backend);
        // A long hold, so "half of it" and "past the original expiry" are wide
        // intervals rather than a scheduling coin toss on a loaded machine.
        const int hold = 400;
        Hl2UnkeyHoldTestAccess::setHoldMs(backend, hold);

        backend.setKeying(true, tx.operation);
        backend.setKeying(false, tx.operation);
        check(Hl2UnkeyHoldTestAccess::dspMuted(backend), "re-key: the hold is armed");

        pumpFor(hold / 2);
        check(Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "re-key: still muted half way through the hold");

        backend.setKeying(true, tx.operation);
        // Well past when the FIRST unkey's hold would have expired.
        pumpFor(hold);
        check(Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "re-key: a key inside the hold cancels it — the demodulator stays "
              "muted past the original expiry");
        check(Hl2UnkeyHoldTestAccess::mixerGateClosed(backend),
              "re-key: and the mixer gate stays closed with it");

        // The second unkey gets its own full window, not the remainder of the
        // first: each unkey has its own T/R turnaround to cover.
        backend.setKeying(false, tx.operation);
        check(Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "re-key: the second unkey arms a fresh hold");
        pumpUntilUnmutedOr(backend, 10 * hold + 500);
        check(!Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "re-key: and that hold expires normally");

        Hl2UnkeyHoldTestAccess::tearDown(backend);
    }

    // ---- 4. THE TX AUDIO MONITOR IS NOT HELD ---------------------------
    //
    // The monitor is the one case that wants to HEAR the transmitter, so there
    // is no T/R turnaround to wait for and no hold belongs on that path. A hold
    // placed there would make a diagnostic that enables the monitor mid-over
    // wait for audio it explicitly asked for.
    {
        TxTestAuthority tx;
        Hl2Backend backend;
        Hl2UnkeyHoldTestAccess::prepare(backend);
        Hl2UnkeyHoldTestAccess::attachProbeDsp(backend);

        backend.setKeying(true, tx.operation);
        check(Hl2UnkeyHoldTestAccess::dspMuted(backend), "monitor: keyed and muted");

        backend.setTxAudioMonitor(true);
        check(!Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "monitor: enabling it mid-transmission unmutes IMMEDIATELY, no hold");
        check(!Hl2UnkeyHoldTestAccess::mixerGateClosed(backend),
              "monitor: and opens the mixer gate, which is what feeds the capture");

        backend.setTxAudioMonitor(false);
        check(Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "monitor: disabling it while keyed re-mutes immediately");

        // Unkeying with the monitor ON must not arm a hold on an unmuted chain.
        backend.setTxAudioMonitor(true);
        backend.setKeying(false, tx.operation);
        check(!Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "monitor: an unkey with the monitor on holds nothing");

        Hl2UnkeyHoldTestAccess::tearDown(backend);
    }

    // ── TURNING THE MONITOR OFF MUST NOT CANCEL AN ARMED HOLD ──────────────
    //
    // Found in review, and it was a real defect in the first version of this
    // change: setTxAudioMonitor's else branch is taken for (keyed, monitor on)
    // AND for (UNKEYED, monitor off), and the second is exactly the state an
    // unkey has just left behind with the hold running and the PA still up.
    // Cancelling it there unmutes inside the T/R turnaround -- the defect this
    // whole change removes, re-entering through another door.
    //
    // NOT HYPOTHETICAL: RadioCertification's run() epilogue calls
    // keyViaOperatorPath(false) and then setTxAudioMonitor(false) in the same
    // synchronous unwind, which is the one path in the tree that deliberately
    // listens to its own transmitter.
    {
        TxTestAuthority tx;
        Hl2Backend backend;
        Hl2UnkeyHoldTestAccess::prepare(backend);
        Hl2UnkeyHoldTestAccess::attachProbeDsp(backend);

        backend.setKeying(true, tx.operation);
        check(Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "monitor-off: keyed and muted");

        backend.setKeying(false, tx.operation);
        check(Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "monitor-off: the unkey armed the hold and we are still muted");

        // The call under test. The monitor was never on; this is the epilogue
        // shape, not an operator asking to hear anything.
        backend.setTxAudioMonitor(false);
        check(Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "monitor-off does NOT cancel an armed hold");
        check(Hl2UnkeyHoldTestAccess::mixerGateClosed(backend),
              "...and the mixer gate stays shut with it");

        // POSITIVE CONTROL. Without this, "still muted" is also what a
        // setTxAudioMonitor that does nothing at all would produce, and what a
        // hold that never expires would produce. Wait the hold out: it must
        // release on its own, which proves the timer was still running rather
        // than merely that nothing unmuted.
        //
        // ON A DEADLINE, NOT ON A FIXED SLEEP. This used to pump for exactly
        // holdMs + 40 and then assert unmuted -- 40 ms of slack on a 70 ms
        // timer, which is the #4703 flake shape under the sanitizer lane. The
        // proof does not need a tight wait: "still muted" is already asserted
        // above, BEFORE the expiry, so all this leg owes is that the release
        // arrives on its own. The same 10x ceiling the other legs use gives
        // that without the coin toss (ten9876, #5850 review).
        pumpUntilUnmutedOr(backend,
                           10 * Hl2UnkeyHoldTestAccess::holdMs(backend) + 500);
        check(!Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "POSITIVE CONTROL: the hold was still RUNNING and expires on its "
              "own -- so the assertion above measured a live hold, not a dead "
              "call");

        Hl2UnkeyHoldTestAccess::tearDown(backend);
    }

    // -- A REDUNDANT UNKEY MUST NOT RESTART THE HOLD -----------------------
    //
    // Found in review (ten9876, #5850). The hold used to be armed by a STATE
    // -- "we are unkeyed and still muted" -- rather than by the key-up EDGE,
    // so every further setKeying(false) that arrived inside the window
    // re-entered the release and restarted the single-shot timer from zero.
    //
    // WHY THAT IS NOT ACADEMIC. applyKeying() is reached with key=false more
    // than once per over on real paths: a tune release followed by the TX
    // coordinator's cleanup unkey, and the automation TX watchdog's forced
    // unkey, both land a second setKeying(false) with m_keyed ALREADY false.
    // The mute could therefore outlive the real MOX-off by up to twice the
    // hold -- and this change's own body charges every millisecond of hold to
    // #5498's post-unkey dropout, so the defect overstates that cost as well
    // as causing it.
    //
    // THIS LEG MEASURES rather than merely asserts, because the number belongs
    // in the report. A long hold keeps "one hold" and "one and a half holds"
    // far apart on a loaded machine.
    {
        TxTestAuthority tx;
        Hl2Backend backend;
        Hl2UnkeyHoldTestAccess::prepare(backend);
        Hl2UnkeyHoldTestAccess::attachProbeDsp(backend);
        const int hold = 200;
        Hl2UnkeyHoldTestAccess::setHoldMs(backend, hold);

        backend.setKeying(true, tx.operation);
        check(Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "double-unkey: keyed and muted");

        // THE REAL KEY-UP EDGE. The stopwatch starts here because this is the
        // MOX-off the hold exists to cover; everything after it is the cost.
        QElapsedTimer sinceRealUnkey;
        backend.setKeying(false, tx.operation);
        sinceRealUnkey.start();
        check(Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "double-unkey: the real edge armed the hold");

        // THE REDUNDANT ONE, half way in, with m_keyed already false. It
        // started no transmission and ends none; it must change nothing.
        pumpFor(hold / 2);
        backend.setKeying(false, tx.operation);
        check(Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "double-unkey: still muted immediately after the redundant unkey");

        pumpUntilUnmutedOr(backend, 10 * hold + 500);
        const qint64 mutedForMs = sinceRealUnkey.elapsed();
        check(!Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "double-unkey: the hold does expire");
        std::printf("      MEASURED: the mute lasted %lld ms past the real "
                    "MOX-off (hold %d ms; a restarted hold would land near "
                    "%d ms)\n",
                    static_cast<long long>(mutedForMs), hold, hold + hold / 2);
        check(mutedForMs >= hold - (hold / 4),
              "double-unkey: and it lasted at least the hold, so the hold ran "
              "at all");
        check(mutedForMs < hold + (hold / 4),
              "double-unkey: a redundant unkey does NOT restart the hold -- the "
              "mute ends one hold after the REAL key-up edge, not one hold "
              "after the last redundant one");

        Hl2UnkeyHoldTestAccess::tearDown(backend);
    }

    // -- TURNING THE MONITOR ON INSIDE AN ARMED HOLD MUST BE ANSWERED NOW ---
    //
    // The other half of the monitor branch, and the half it used to swallow
    // (ten9876, #5850). The guard that stops a monitor-OFF from cancelling an
    // armed hold was written on (!m_keyed && hold armed) alone, so it was
    // taken for BOTH values of `on`: a diagnostic that asked to HEAR the
    // receiver inside the window got silence until the timer expired.
    //
    // ASKING TO HEAR IS ASKING FOR SOMETHING. Only asking for OFF is asking
    // for nothing, and only that one may be answered by doing nothing.
    {
        TxTestAuthority tx;
        Hl2Backend backend;
        Hl2UnkeyHoldTestAccess::prepare(backend);
        Hl2UnkeyHoldTestAccess::attachProbeDsp(backend);
        const int hold = 400;   // wide, so "inside the window" is not a race
        Hl2UnkeyHoldTestAccess::setHoldMs(backend, hold);

        backend.setKeying(true, tx.operation);
        backend.setKeying(false, tx.operation);
        check(Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "monitor-on-in-window: the unkey armed the hold");

        backend.setTxAudioMonitor(true);
        check(!Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "monitor ON inside an armed hold unmutes IMMEDIATELY, it does "
              "not wait the hold out");
        check(!Hl2UnkeyHoldTestAccess::mixerGateClosed(backend),
              "...and opens the mixer gate, which is what actually feeds the "
              "capture");

        // AND THE HOLD IS CANCELLED, not merely overridden. A key-down with
        // the monitor on takes the `!muteWhileKeyed` path, so a mute left
        // standing here would be re-armed into a FRESH hold on a KEY-DOWN --
        // the second half of the same defect.
        backend.setKeying(true, tx.operation);
        check(!Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "a key-down with the monitor on finds no stale mute to re-arm a "
              "hold from");
        pumpFor(hold / 4);
        check(!Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "...and stays unmuted, which is what the monitor was turned on "
              "for");

        Hl2UnkeyHoldTestAccess::tearDown(backend);
    }

    // ══ CW FULL BREAK-IN SKIPS THE HOLD — THE RULING, ASSERTED BOTH WAYS ══
    //
    // ON8ST ruled the question this PR disclosed and did not decide: when CW
    // full break-in is on, the unkey hold does not arm. An operator who turns
    // QSK on has asked to hear between elements and accepts the leak.
    //
    // THE PREDICATE, AND WHY IT IS NOT WHAT THE THREAD SAID IT WAS. The body
    // said "applyKeying already receives cwBreakIn". It receives it on the
    // key-DOWN only: setCwKeying()'s down branch is the one and only site that
    // passes true, and the key-UP arrived through the CW hang timer calling
    // setKeying(), which hard-codes false. Gating the hold on the parameter as
    // it stood would have been DEAD CODE in exactly the case it is for. The
    // hang timer now calls applyKeying() directly with cwBreakIn true, which is
    // wire-identical on an unkey (MetisClient::setMoxImpl reads the flag only
    // in its `keyed ?` arm).
    //
    // THE ~17 WPM CROSSOVER IS ARITHMETIC AND IS NOT ASSERTED HERE. There is no
    // CW measurement behind any of this — #5497's eleven windows are 4-second
    // SSB keys. What these legs assert is the BEHAVIOUR: armed, or not armed.

    // -- 7a. BREAK-IN ON: an element release arms NO hold ------------------
    {
        TxTestAuthority tx;
        Hl2Backend backend;
        Hl2UnkeyHoldTestAccess::prepare(backend);
        Hl2UnkeyHoldTestAccess::attachProbeDsp(backend);
        Hl2UnkeyHoldTestAccess::setTxReceiverMode(backend, "CW");
        // A hold far longer than the shipped 70 ms, so "no hold armed" and "the
        // hold armed" are hundreds of milliseconds apart rather than a
        // scheduling coin toss. The 6 ms CW envelope hang is the only delay
        // that should appear.
        const int hold = 400;
        Hl2UnkeyHoldTestAccess::setHoldMs(backend, hold);

        // ELEMENT KEY-DOWN, full break-in. This is the site that raises MOX.
        backend.setCwKeying(true, /*breakIn=*/true, /*breakInDelayMs=*/0, tx.operation);
        check(Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "QSK: an element key-down mutes the demodulator, exactly as a "
              "voice key does");
        check(Hl2UnkeyHoldTestAccess::mixerGateClosed(backend),
              "QSK: and closes the mixer gate with it");

        // ELEMENT KEY-UP. The release starts the CW hang (6 ms at cwDelay 0);
        // when that fires, the MOX-off goes out and — under the ruling — the
        // receiver opens immediately behind it.
        QElapsedTimer sinceElementUp;
        backend.setCwKeying(false, /*breakIn=*/true, /*breakInDelayMs=*/0, tx.operation);
        sinceElementUp.start();
        pumpUntilUnmutedOr(backend, 10 * hold + 500);
        const qint64 openedAfterMs = sinceElementUp.elapsed();

        check(!Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "QSK: the receiver is OPEN after the element release");
        check(!Hl2UnkeyHoldTestAccess::mixerGateClosed(backend),
              "QSK: and the mixer gate is open, so audio actually flows");
        // THE STRUCTURAL CLAIM, clock-free. "Unmuted" could also be a hold that
        // was armed and has already expired; "no timer running" cannot.
        check(!Hl2UnkeyHoldTestAccess::holdArmed(backend),
              "QSK: NO HOLD IS ARMED by an element release — the ruling, "
              "asserted on the timer and not on the mute");
        check(Hl2UnkeyHoldTestAccess::moxOffApplied(backend),
              "QSK: and the MOX-off did go out, so this is a real unkey and not "
              "a key that never happened");
        std::printf("      MEASURED: the receiver opened %lld ms after the "
                    "element key-up (hold %d ms; with the hold armed it could "
                    "not open before %d ms)\n",
                    static_cast<long long>(openedAfterMs), hold, hold);
        check(openedAfterMs < hold / 2,
              "QSK: and it opened in a small fraction of the hold, so the hold "
              "was skipped rather than merely short");

        Hl2UnkeyHoldTestAccess::tearDown(backend);
    }

    // -- 7b. BREAK-IN ON: the receiver is open BETWEEN elements ------------
    //
    // The operator-visible claim, on the SHIPPED constant rather than an
    // inflated one, at a speed above the arithmetic crossover. 25 WPM is 48 ms
    // of inter-element space at PARIS timing; the shipped hold is 70 ms, so on
    // the pre-ruling behaviour this window was closed for the whole of it.
    {
        TxTestAuthority tx;
        Hl2Backend backend;
        Hl2UnkeyHoldTestAccess::prepare(backend);
        Hl2UnkeyHoldTestAccess::attachProbeDsp(backend);
        Hl2UnkeyHoldTestAccess::setTxReceiverMode(backend, "CW");
        check(Hl2UnkeyHoldTestAccess::holdMs(backend)
                  == Hl2UnkeyHoldTestAccess::defaultHoldMs(),
              "between-elements: this leg runs on the SHIPPED hold, not a test "
              "value");

        const int interElementMs = 48;   // 1200/25 WPM, PARIS

        backend.setCwKeying(true, true, 0, tx.operation);
        backend.setCwKeying(false, true, 0, tx.operation);
        pumpUntilUnmutedOr(backend, 10 * Hl2UnkeyHoldTestAccess::defaultHoldMs() + 500);
        check(!Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "between-elements: open after element 1");

        // THE WHOLE SPACE, not just its start. A hold that armed late would
        // close the window again part way through it.
        pumpFor(interElementMs);
        check(!Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "between-elements: STILL open a full 48 ms inter-element space "
              "later — at 25 WPM the QSK operator hears the band");
        check(!Hl2UnkeyHoldTestAccess::holdArmed(backend),
              "between-elements: and nothing armed a hold behind our back");

        // ELEMENT 2 mutes again, which is the half that must not be lost.
        backend.setCwKeying(true, true, 0, tx.operation);
        check(Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "between-elements: the next element key-down mutes again — the "
              "leak is accepted BETWEEN elements, not DURING one");

        Hl2UnkeyHoldTestAccess::tearDown(backend);
    }

    // -- 8. BREAK-IN OFF: the hold still arms and still runs its length -----
    //
    // SEMI-BREAK-IN IS THE OTHER HALF OF THE RULING and it is asserted, not
    // assumed. With break-in off setCwKeying() never keys: CW rides an MOX the
    // operator asserted, and the over ends when THEY release it. That release
    // arrives through setKeying() with cwBreakIn false, has a real T/R
    // turnaround behind it, and must get the full hold.
    {
        TxTestAuthority tx;
        Hl2Backend backend;
        Hl2UnkeyHoldTestAccess::prepare(backend);
        Hl2UnkeyHoldTestAccess::attachProbeDsp(backend);
        Hl2UnkeyHoldTestAccess::setTxReceiverMode(backend, "CW");
        const int hold = 200;
        Hl2UnkeyHoldTestAccess::setHoldMs(backend, hold);

        // The operator asserts MOX. This is what break-in OFF means.
        backend.setKeying(true, tx.operation);
        check(Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "semi: the operator's MOX mutes the receiver");

        // Elements inside that MOX. They must not touch the mute in either
        // direction — and must not arm a hold when they are released.
        backend.setCwKeying(true, /*breakIn=*/false, /*breakInDelayMs=*/0, tx.operation);
        check(Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "semi: an element inside the over changes nothing");
        backend.setCwKeying(false, /*breakIn=*/false, /*breakInDelayMs=*/0, tx.operation);
        pumpFor(20);   // well past the 6 ms envelope hang, had one been started
        check(Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "semi: and its release does NOT open the receiver mid-over");
        check(!Hl2UnkeyHoldTestAccess::holdArmed(backend),
              "semi: an element release with break-in off arms no hold either — "
              "it ended no transmission");

        // THE REAL UNKEY: the operator drops MOX.
        QElapsedTimer sinceRealUnkey;
        backend.setKeying(false, tx.operation);
        sinceRealUnkey.start();
        check(Hl2UnkeyHoldTestAccess::holdArmed(backend),
              "semi: THE HOLD STILL ARMS — only full break-in skips it");
        check(Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "semi: and the receiver is held shut past the MOX-off");
        check(Hl2UnkeyHoldTestAccess::moxOffApplied(backend),
              "semi: with the MOX-off already applied, which is the ordering "
              "#5497 is about");

        pumpUntilUnmutedOr(backend, 10 * hold + 500);
        const qint64 mutedForMs = sinceRealUnkey.elapsed();
        std::printf("      MEASURED: semi-break-in held the mute %lld ms past "
                    "the MOX-off (hold %d ms)\n",
                    static_cast<long long>(mutedForMs), hold);
        check(mutedForMs >= hold - (hold / 4),
              "semi: THE HOLD RAN ITS FULL LENGTH — the CW branch did not "
              "shorten it");
        check(!Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "semi: and it expires normally");

        Hl2UnkeyHoldTestAccess::tearDown(backend);
    }

    // -- 9. THE DOUBLE-UNKEY GUARD IS UNAFFECTED, BOTH WAYS ----------------
    //
    // The edge guard from the last round (ten9876, #5850) stops a redundant
    // setKeying(false) restarting the single-shot timer. The CW branch is a
    // NEW arm of the same `if`, so the question is whether it changed the
    // guard's behaviour on either side of the break-in switch. Leg 8 above
    // already re-measures the semi path under a full hold; this leg drives a
    // redundant unkey through both.
    {
        // 9a. WITH BREAK-IN: a redundant unkey after a QSK element release
        // must still arm nothing. This is the shape the tune-release and
        // watchdog paths produce, arriving on a chain that is already open.
        TxTestAuthority tx;
        Hl2Backend backend;
        Hl2UnkeyHoldTestAccess::prepare(backend);
        Hl2UnkeyHoldTestAccess::attachProbeDsp(backend);
        Hl2UnkeyHoldTestAccess::setTxReceiverMode(backend, "CW");
        const int hold = 300;
        Hl2UnkeyHoldTestAccess::setHoldMs(backend, hold);

        backend.setCwKeying(true, true, 0, tx.operation);
        backend.setCwKeying(false, true, 0, tx.operation);
        pumpUntilUnmutedOr(backend, 10 * hold + 500);
        check(!Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "double-unkey/QSK: open after the element release");

        // The redundant one. m_keyed is already false and the chain is already
        // unmuted, so this reaches the third arm of the release branch.
        backend.setKeying(false, tx.operation);
        check(!Hl2UnkeyHoldTestAccess::holdArmed(backend),
              "double-unkey/QSK: a redundant unkey on an open chain arms no "
              "hold");
        pumpFor(hold / 4);
        check(!Hl2UnkeyHoldTestAccess::dspMuted(backend),
              "double-unkey/QSK: ...and does not close the receiver behind the "
              "operator");

        Hl2UnkeyHoldTestAccess::tearDown(backend);
    }
    {
        // 9b. WITHOUT BREAK-IN, re-measured with the CW branch present. This
        // is leg 6's assertion again, and it is repeated rather than trusted:
        // the branch it now shares an `if` with is new, and the whole point of
        // the guard is that the mute ends one hold after the REAL edge.
        TxTestAuthority tx;
        Hl2Backend backend;
        Hl2UnkeyHoldTestAccess::prepare(backend);
        Hl2UnkeyHoldTestAccess::attachProbeDsp(backend);
        const int hold = 200;
        Hl2UnkeyHoldTestAccess::setHoldMs(backend, hold);

        backend.setKeying(true, tx.operation);
        QElapsedTimer sinceRealUnkey;
        backend.setKeying(false, tx.operation);
        sinceRealUnkey.start();
        check(Hl2UnkeyHoldTestAccess::holdArmed(backend),
              "double-unkey/no-QSK: the real edge armed the hold");

        pumpFor(hold / 2);
        backend.setKeying(false, tx.operation);
        pumpUntilUnmutedOr(backend, 10 * hold + 500);
        const qint64 mutedForMs = sinceRealUnkey.elapsed();
        std::printf("      MEASURED: with the CW branch present, the mute "
                    "lasted %lld ms past the real MOX-off (hold %d ms; a "
                    "restarted hold would land near %d ms)\n",
                    static_cast<long long>(mutedForMs), hold, hold + hold / 2);
        check(mutedForMs < hold + (hold / 4),
              "double-unkey/no-QSK: the edge guard is UNAFFECTED by the CW "
              "branch — still one hold after the real key-up edge");

        Hl2UnkeyHoldTestAccess::tearDown(backend);
    }

    std::printf("\n  %s\n\n", g_failures == 0 ? "all checks passed" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
