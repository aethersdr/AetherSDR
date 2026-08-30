#pragma once

namespace AetherSDR {

// Which producer owns the Client-Side QSO recorder's TX slot (#2539, #4281).
//
// Two sources can reach QsoRecorder::feedTxAudio: the mic-driven post-limiter
// monitor tap (AudioEngine::txFinalMonitorPcmReady) and the CW record pump
// (AudioEngine::cwSidetoneRecordPcmReady).  The recorder writes whatever
// arrives while its TX gate is open and cannot tell the two apart, so exactly
// one of them must be feeding it at any instant — otherwise the WAV
// interleaves both, block by block, and the last writer of each block wins.
//
// One question decides the owner: is the radio keyed for an over that OUR
// keyer is sending?  It is deliberately NOT a function of whether the PC mic
// capture stream happens to be open.  Mic capture starts when mic_selection
// becomes "PC" and stays up across mode changes (MainWindow_Session.cpp), so
// on an ordinary client setup it is running throughout a CW over and says
// nothing about who should be recorded.  Gating the pump on the mic stream
// being down was #4281: the pump never ran, no sidetone was recorded, and the
// still-running mic tap wrote room noise into the CW portion of the file.
enum class TxRecorderSource {
    None,        // not transmitting — the recorder's TX gate is shut
    Mic,         // phone/SSB over: the post-limiter mic monitor tap feeds it
    CwSidetone,  // our CW/CWX over: the record pump feeds it
};

// radioTransmitting: the radio's interlock TX state (any owner).
// cwKeyedThisOver:   latched when our own keyer fires during this over, so a
//                    voice/DAX/tune over never selects the CW source, and
//                    another client's CW never selects it either.
constexpr TxRecorderSource txRecorderSource(bool radioTransmitting,
                                            bool cwKeyedThisOver)
{
    // cwKeyedThisOver is tested FIRST, and deliberately without regard to the
    // interlock. Under break-in the radio drops the interlock between EVERY
    // element — measured on a FLEX-8400 at 20 WPM: 47 false-edges in 15.8 s,
    // gaps a median 52 ms long. Asking "is the radio keyed right now" therefore
    // answers "no" throughout every inter-element gap, and an earlier version of
    // this function returned None there. That let the mic tap back into the
    // recorder in every gap (audible as the operator's voice under the CW) and
    // stopped the pump emitting the silence that preserves morse spacing.
    //
    // The over is the unit of ownership, not the element. The latch spans it:
    // set on our first key-down, aged out by the pump once the elements stop
    // AND the radio is back at RX (cwLatchShouldAge).
    if (cwKeyedThisOver)   return TxRecorderSource::CwSidetone;
    if (radioTransmitting) return TxRecorderSource::Mic;
    return TxRecorderSource::None;
}

// The two call sites' predicates, so the "exactly one owner" rule is stated
// once rather than re-derived at each end.
constexpr bool cwRecordPumpOwnsRecorder(TxRecorderSource s)
{
    return s == TxRecorderSource::CwSidetone;
}
constexpr bool micTapOwnsRecorder(TxRecorderSource s)
{
    return s != TxRecorderSource::CwSidetone;
}

// Whether the pump should spend the render at all. Ownership above answers
// WHOSE audio belongs in the file; this adds the orthogonal question of whether
// there IS a file — with none open, QsoRecorder::feedTxAudio discards every
// block, so rendering is pure waste on the audio thread (#4281).
//
// Deliberately a SEPARATE function rather than a third parameter to
// txRecorderSource(): ownership must stay a two-input contract, because the
// defect this file exists to prevent was exactly an extra input smuggled into
// that decision. The test pins that shape.
// How long after the last CW key EDGE the over is considered finished.
//
// Must outlast the longest silence WITHIN an over — the inter-word gap, 7 dit
// units — and no longer: for its whole duration the recorder holds RX audio off
// so the pump's silence is not interleaved with receive audio, so every extra
// millisecond is a millisecond of the other station's reply missing from the
// file. In QSK they answer within 200-400 ms.
//
// 8 units = inter-word gap + 1 unit of margin. A dit is 1200/WPM ms.
constexpr int kCwOverHangUnits = 8;
constexpr long long cwOverHangMs(int wpm)
{
    return kCwOverHangUnits * 1200LL / (wpm > 0 ? wpm : 20);
}

constexpr bool cwRecordPumpShouldRender(TxRecorderSource s, bool recordingOpen)
{
    return cwRecordPumpOwnsRecorder(s) && recordingOpen;
}

// Whether the CW-over latch may age out now. The pump asks this on every tick
// once our keyer has fired.
//
// Two conditions, both required. The stopwatch — gapMs since the last key EDGE
// exceeds the over-hang — is what ends the over under break-in, where the
// radio drops the interlock in every inter-element gap and the interlock alone
// would say "finished" dozens of times per over. The interlock — the radio
// back at RX — is what ends it everywhere else: with break-in off, or with a
// break-in delay longer than the hang, the radio holds TX across gaps the
// stopwatch has already given up on, and a latch aged out under a live
// interlock hands the recorder's TX slot to the mic tap
// (txRecorderSource(true, false) == Mic) in the middle of our own over. That
// is #4281's symptom returning one pause at a time, at any speed where the
// hang is shorter than the radio's delay: 320 ms at 30 WPM against a delay
// slider that runs to 2000.
//
// Cost, accepted: a transmission that raises the interlock inside the hang
// for some OTHER reason — a voice over begun within 8 dit units of the last
// CW element — keeps the latch, and so the pump, until the radio returns to
// RX. Not producible by hand (a mode change plus PTT inside ~half a second);
// stated in the PR rather than designed around.
constexpr bool cwLatchShouldAge(bool radioTransmitting, long long gapMs,
                                long long hangMs)
{
    return !radioTransmitting && gapMs > hangMs;
}

// Whether an over's END should arm the recorder's idle-stop countdown. The
// recorder has two over sources — MOX (voice) and the CW gate — whose end
// edges can interleave: the CW gate-close is queued and the over-hang lets a
// voice over begin up to a hang before it lands, so the close can arrive
// INSIDE a live voice over. Arming the countdown then would auto-stop that
// recording mid-transmission once the (user-configurable, floor 10 s) timeout
// elapses. The countdown may start only once BOTH sources are down; whichever
// over ends last arms it.
constexpr bool idleCountdownShouldArm(bool recording, bool transmitting,
                                      bool cwOverActive)
{
    return recording && !transmitting && !cwOverActive;
}

} // namespace AetherSDR
