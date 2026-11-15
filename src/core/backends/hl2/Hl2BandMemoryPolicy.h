#pragma once

// Per-band LNA memory: which value a session comes up on, and which value the
// band memory records when the operator leaves that band. A connect that pins
// the LNA via the lnaGainDb param must not overwrite the start band's stored
// entry on the first band change (Hl2Backend::rememberCurrentBandState). Kept in
// a header so tests exercise the same expressions the backend runs. Ordering
// lives in Hl2Backend::connectRadio and ::applyPerBandStateFor.

namespace AetherSDR::hl2 {

// A clamp local to this header so the decision is testable without pulling in
// the backend's translation unit. Mirrors qBound's argument order.
constexpr int clampDb(int minDb, int v, int maxDb)
{
    return v < minDb ? minDb : (v > maxDb ? maxDb : v);
}

// Native HL2 gain format: address 0x0a bit 6 selects the six-bit AD9866
// code, covering -12..+48 dB. See Protocol.md and ad9866.v at 883a338.
constexpr int kLnaGainMinDb = -12;
constexpr int kLnaGainMaxDb = 48;
constexpr int kLnaGainStepDb = 1;
constexpr int kLnaDefaultGainDb = 20;

// What a session comes up on for the start band.
struct ConnectLna {
    int liveDb = 0;
    // True when liveDb came from the connect param while the start band also had a
    // stored entry: a session pin the operator never chose for this band.
    bool sessionPin = false;
};

inline ConnectLna connectLna(bool haveRestoredState,
                             bool hasStoredEntry, int storedDb,
                             bool paramPresent, int paramDb,
                             int defaultDb, int minDb, int maxDb)
{
    ConnectLna out;
    // The explicit param wins the live value: an automation or test caller pins
    // the gain outright.
    if (paramPresent) {
        // The reported gain and session pin must use the same bounded value
        // as the wire encoder. Compare after clamping so an out-of-range
        // request equal to the stored endpoint does not create a false pin.
        out.liveDb = clampDb(minDb, paramDb, maxDb);
        out.sessionPin = haveRestoredState && hasStoredEntry && out.liveDb != storedDb;
        return out;
    }
    if (haveRestoredState) {
        out.liveDb = clampDb(minDb, hasStoredEntry ? storedDb : defaultDb, maxDb);
        return out;
    }
    out.liveDb = defaultDb;
    return out;
}

// What rememberCurrentBandState() records for the band being left: the live
// value, except for a session pin over an existing entry, where the stored entry
// is kept. The pin still drives the radio; only its persistence is refused.
inline int bandMemoryWriteback(int liveDb, bool sessionPin,
                               bool hasStoredEntry, int storedDb)
{
    if (sessionPin && hasStoredEntry) {
        return storedDb;
    }
    return liveDb;
}

}  // namespace AetherSDR::hl2
