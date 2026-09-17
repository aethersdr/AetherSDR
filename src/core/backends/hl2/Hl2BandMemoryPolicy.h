#pragma once

// Per-band LNA memory: which value a session comes up on, and which value the
// band memory records when the operator leaves that band.
//
// These are two separate questions and the backend previously answered only the
// first. The second is where the defect lives: a connect that pins the LNA via
// the namespaced lnaGainDb param diverges the live value from the start band's
// stored entry, and the FIRST band change then writes the live value back over
// that entry (Hl2Backend::rememberCurrentBandState). The operator's calibration
// for that band is gone, replaced by a number that was only ever meant to hold
// for one session.
//
// Symptom, and why it is worth a header: the loss is silent and it is delayed.
// Nothing is wrong at connect — the pinned value is what was asked for. The
// stored entry dies later, on an unrelated action, and the next session comes
// up on the pinned value as though the operator had chosen it. By the time a
// band sounds wrong there is nothing left on disk that says what it used to be.
//
// They live in a header, evaluated by Hl2Backend rather than copied into it, so
// the suite exercises the SAME expressions the backend runs — the reasoning
// Hl2TxLevelPolicy.h states, and the same reason it applies here: a test
// against a re-typed copy of this decision would agree with itself while the
// backend kept the bug.
//
// See Hl2Backend::connectRadio and ::applyPerBandStateFor for the surrounding
// ordering; this header is the decision only.

namespace AetherSDR::hl2 {

// A clamp local to this header so the decision is testable without pulling in
// the backend's translation unit. Mirrors qBound's argument order.
constexpr int clampDb(int minDb, int v, int maxDb)
{
    return v < minDb ? minDb : (v > maxDb ? maxDb : v);
}

// ── THE AD9866's RANGE, DECLARED HERE AND NOWHERE ELSE ────────────────────
//
// Hl2Backend::kLnaGainMinDb/kLnaGainMaxDb/kLnaDefaultGainDb alias these rather
// than restating them, and the suite reads THESE rather than a hand-typed copy.
// That is not tidiness: hl2_band_memory_test carried its own `kMax = 48` and
// went on passing when the backend's ceiling moved to 19, asserting a range the
// hardware no longer has -- the exact failure this header's opening paragraph
// names. Caught by aethersdr-agent on #5752.
//
// +19 is where the register stops meaning what it says. ccRxGain encodes dB as
// `code = db + 12` and the gateware decodes five bits, so code 32 (which is
// +20 dB asked for) is read as code 0 and delivers -12 dB. 31 is the last code
// that survives the decode, and 31 - 12 = 19.
constexpr int kLnaGainMinDb     = -12;
constexpr int kLnaGainMaxDb     = 19;
constexpr int kLnaGainStepDb    = 1;
constexpr int kLnaDefaultGainDb = 0;

// WHAT A STORED GAIN ABOVE THE CEILING MEANT WHEN IT WAS WRITTEN.
//
// Lowering the ceiling from 48 to 19 re-points every value an operator already
// has on disk in 20..48, and CLAMPING them is the one option that changes what
// the hardware does without telling anyone: a stored 20 was being applied as
// -12 dB (code 32 folds to code 0), and clamping it to 19 would hand that
// operator +31 dB on the first connect after an update -- precisely the "loud
// audio and a plausible ADC overload, for operators who changed nothing" that
// Hl2Backend.h gives as its reason for NOT restoring +19 as the fresh default.
//
// So a legacy value is MIGRATED to what the radio was actually doing with it,
// through the old path exactly: clamp the code at 60 the way the old ccRxGain
// did, fold it the way the gateware does, and read it back as dB. The result
// is always inside [-12, 19] because `(code & 0x1F) - 12` is.
//
//   stored 20 -> code 32 -> 0  -> -12 dB      stored 32 -> code 44 -> 12 -> 0 dB
//   stored 24 -> code 36 -> 4  ->  -8 dB      stored 48 -> code 60 -> 28 -> +16 dB
//
// The operator hears exactly what they heard before the update, and the slider
// finally agrees with it. That is the migrate idiom the restore loader already
// uses for the pre-#4914 CW passband, rather than the drop idiom it uses for an
// out-of-range mic level -- dropping would be right if the stored value were
// meaningless, and it is not: it is a faithful record of a number the radio
// folded.
constexpr int migrateStoredLnaDb(int storedDb)
{
    if (storedDb >= kLnaGainMinDb && storedDb <= kLnaGainMaxDb) {
        return storedDb;
    }
    if (storedDb < kLnaGainMinDb) {
        // Never reachable through the old encode -- codes are clamped at 0 --
        // so there is nothing to reconstruct and the floor is the honest answer.
        return kLnaGainMinDb;
    }
    // WIDENED BEFORE THE ADD, not clamped after it. `storedDb + 12` overflows
    // for storedDb near INT_MAX, and signed overflow is undefined rather than
    // wrapping -- so the clamp that looks like it bounds the input runs on a
    // value the standard says does not exist. Reproduced under UBSan by
    // aethersdr-agent on #5752 with migrateStoredLnaDb(INT_MAX).
    //
    // No stored document can hold INT_MAX; that is not the point. A pure
    // function on an int should be total on an int, because the next caller is
    // the one that will not have checked.
    const long long raw = static_cast<long long>(storedDb) + 12;
    const int oldCode = static_cast<int>(raw < 0 ? 0 : (raw > 60 ? 60 : raw));
    return (oldCode & 0x1F) - 12;
}

// What a session comes up on for the start band.
struct ConnectLna {
    int liveDb = 0;
    // TRUE when liveDb came from the connect param while the start band ALSO
    // had a stored entry — i.e. the live value is a session pin that the
    // operator never chose for this band. Purely informational to the caller;
    // it is bandMemoryWriteback below that decides what it costs.
    bool sessionPin = false;
};

inline ConnectLna connectLna(bool haveRestoredState,
                             bool hasStoredEntry, int storedDb,
                             bool paramPresent, int paramDb,
                             int defaultDb, int minDb, int maxDb)
{
    ConnectLna out;
    // The explicit param still wins the LIVE value. That precedence is
    // deliberate and documented at the call site: an automation or test caller
    // pins the gain outright, and a stored entry must not silently ignore what
    // the caller asked for. This header does not reverse it.
    if (paramPresent) {
        // CLAMPED, WHICH IT WAS NOT. The comment here used to say "preserve the
        // pre-existing explicit-parameter behavior; this PR changes
        // persistence, not the connect parameter's range handling" -- a
        // deliberate deferral, and harmless while the ceiling was +48, because
        // ccRxGain's own clamp caught anything higher on the way to the wire.
        //
        // It stopped being harmless when the ceiling became +19 (the last code
        // before the AD9866's `code & 0x1F` fold). An unclamped param then PINS
        // a value the radio will never apply: connect with lnaGainDb=20 and the
        // session pins 20 while the hardware runs 19, so the pinned value and
        // the live value disagree for the whole session. An operator who then
        // writes "the pinned value" writes something the pin does not
        // recognise.
        //
        // Clamping here makes the pin a statement about the radio rather than
        // about the request. The comparison below uses the CLAMPED value for
        // the same reason: a param of +48 and a stored +19 are the same applied
        // gain and must not be read as a divergence worth pinning.
        out.liveDb = clampDb(minDb, paramDb, maxDb);
        out.sessionPin =
            haveRestoredState && hasStoredEntry && out.liveDb != storedDb;
        return out;
    }
    if (haveRestoredState) {
        out.liveDb = clampDb(minDb, hasStoredEntry ? storedDb : defaultDb, maxDb);
        return out;
    }
    out.liveDb = defaultDb;
    return out;
}

// What rememberCurrentBandState() should record for the band being left.
//
// Normally the live value: leaving a band records what the operator set while
// they were on it, which is the whole point of the memory.
//
// The exception is a session pin. That value came from the connect param, not
// from the operator acting on this band, and the band already had an entry of
// its own — so recording it would overwrite a calibration with a number nobody
// chose for this band. The stored entry is kept instead.
//
// Note what this deliberately does NOT do: it does not make the pin invisible.
// The live gain stays pinned, the radio runs at the requested value, and every
// pan is told about it. Only the persistence is refused, because persistence is
// the part that outlives the session that asked for it.
inline int bandMemoryWriteback(int liveDb, bool sessionPin,
                               bool hasStoredEntry, int storedDb)
{
    if (sessionPin && hasStoredEntry) {
        return storedDb;
    }
    return liveDb;
}

}  // namespace AetherSDR::hl2
