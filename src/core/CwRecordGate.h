#pragma once

namespace AetherSDR {

// Which producer owns the Client-Side QSO recorder's TX slot (#2539, #4281).
// QsoRecorder::feedTxAudio can't tell the mic monitor tap
// (txFinalMonitorPcmReady) from the CW record pump (cwSidetoneRecordPcmReady),
// so exactly one may feed it at a time. The owner is decided by whether the radio
// is keyed for an over OUR keyer is sending, never by whether PC mic capture is
// open (it stays up across mode changes).
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
    // cwKeyedThisOver is tested first, regardless of the interlock: under break-in
    // the radio drops the interlock between every element (FLEX-8400 at 20 WPM: 47
    // false edges in 15.8 s, median gap 52 ms). The over is the unit of ownership;
    // the latch is set on our first key-down and aged by the pump (cwLatchShouldAge).
    if (cwKeyedThisOver) {
        return TxRecorderSource::CwSidetone;
    }
    if (radioTransmitting) {
        return TxRecorderSource::Mic;
    }
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

// Over-scoped hang: must outlast the inter-word gap at the SLOWEST speed keyed in
// the over. CWX keys at CwxModel's per-segment wpm, independent of
// TransmitModel::cwSpeed (15 WPM against a 30 WPM mirror = 560 ms gap vs 320 ms
// hang, #4281). overrideWpm is the slowest speed announced for the current over
// (0 = none, a paddle over); the slower speed always wins, since too short splits
// the over and too long only trims the recording's tail.
constexpr long long cwOverHangMs(int mirrorWpm, int overrideWpm)
{
    return (overrideWpm > 0 && (mirrorWpm <= 0 || overrideWpm < mirrorWpm))
        ? cwOverHangMs(overrideWpm)
        : cwOverHangMs(mirrorWpm);
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
constexpr bool cwRecordPumpShouldRender(TxRecorderSource s, bool recordingOpen)
{
    return cwRecordPumpOwnsRecorder(s) && recordingOpen;
}

// Whether the radio's transmission is OUR CW over's. radioTransmitting alone is
// the any-owner interlock, which would let other clients' TX, a TUNE inside the
// hang, or a stray key edge during voice drive our state.
// - txOwnedByUs: tx_client_handle equals ours, parsed with the interlock and
//   rising with it (valid under break-in, which has no MOX edge). A missing
//   handle parses as ours (RadioModel treats 0 as ours), so our own over is
//   never dropped.
// - tuneActive excludes our TUNE/two-tone carrier; TransmitModel sets it before
//   the tune command is sent, so it can't lose a race with the interlock.
constexpr bool cwOverTxActive(bool radioTransmitting, bool txOwnedByUs,
                              bool tuneActive)
{
    return radioTransmitting && txOwnedByUs && !tuneActive;
}

// Whether the CW-over latch may age out now; asked on every pump tick once our
// keyer has fired. Both required: gapMs since the last key edge exceeds the hang
// (ends the over under break-in), and ourTxActive (cwOverTxActive(...), not the
// raw interlock) is false, so a break-in delay longer than the hang doesn't hand
// the slot to the mic tap mid-over. Foreign or tune TX doesn't hold the over.
// Accepted residual: unattributed foreign TX (no tx_client_handle) parses as
// ours and keeps the latch.
constexpr bool cwLatchShouldAge(bool ourTxActive, long long gapMs,
                                long long hangMs)
{
    return !ourTxActive && gapMs > hangMs;
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
