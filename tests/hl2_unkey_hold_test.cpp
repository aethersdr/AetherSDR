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
// WHAT IT DOES NOT ASSERT, said here rather than left to be discovered: any
// audio level, anything about the HL2's actual T/R turnaround, and whether 70
// ms is the right number. Those are bench questions and #5497 carries the
// measurements. This is an ordering test with a clock in it.
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

    std::printf("\n  %s\n\n", g_failures == 0 ? "all checks passed" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
