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
    if (!radioTransmitting) return TxRecorderSource::None;
    return cwKeyedThisOver ? TxRecorderSource::CwSidetone
                           : TxRecorderSource::Mic;
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
constexpr bool cwRecordPumpShouldRender(TxRecorderSource s, bool recordingOpen)
{
    return cwRecordPumpOwnsRecorder(s) && recordingOpen;
}

} // namespace AetherSDR
