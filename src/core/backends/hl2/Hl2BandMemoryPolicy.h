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
        out.liveDb = paramDb;
        out.sessionPin = haveRestoredState && hasStoredEntry && paramDb != storedDb;
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
