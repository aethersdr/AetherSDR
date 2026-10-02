#pragma once

#include <QString>

// Decides how to recover a default slice when "slice list" returns empty at
// GUI-attach. A radio restoring our persistent client_id can bring back our
// panadapter without its slice (#3212); the pan is already claimed by decision
// time, so creating a new panafall would duplicate it. Pure and header-only so
// it is unit-testable; RadioModel feeds it state and acts on the Decision.

namespace AetherSDR::SliceRecreatePolicy {

enum class Action {
    // The radio already restored one of our panadapters; attach the slice to it
    // instead of creating a new pan. Prevents the #3212 duplicate panadapter.
    ReuseRestoredPan,
    // No owned panadapter exists yet (true first-connect / standalone): create a
    // fresh panafall and then a slice on it.
    CreateNewPan,
};

// Runtime state captured by RadioModel at "slice list -> (empty)" time.
struct Inputs {
    // True when m_activePanId names a panadapter we already hold in
    // m_panadapters (i.e. the radio restored it for us).
    bool hasRestoredPan{false};
    // The restored pan's center frequency in MHz, as last reported by the radio
    // ("display pan ... center="). PanadapterModel initialises m_centerMhz to
    // 14.1, so a just-claimed pan reports a positive center rather than 0 — but
    // a zero/malformed "center=" status still parses (unchecked toDouble in
    // applyPanStatus) to 0.0, so decide()'s <= 0 fallback paths remain reachable
    // (see testRestoredPanCenterZeroFallsBack).
    double restoredPanCenterMhz{0.0};
    // Client-persisted last frequency (AppSettings "LastFrequency"). <= 0 means
    // unset. Used only as a fallback — see decide().
    double lastFreqMhz{0.0};
    // Client-persisted last mode (AppSettings "LastMode"). Empty means unset.
    QString lastMode;
};

struct Decision {
    Action action{Action::CreateNewPan};
    double freqMhz{14.225000};
    QString mode{QStringLiteral("USB")};
    QString antenna{QStringLiteral("ANT1")};
};

// Pure decision, no I/O. Reuse path: put the slice at the restored pan's own
// center (radio-authoritative); LastFrequency may be on another band entirely
// and land outside the span. LastFrequency / 14.225 are fallbacks only until
// the pan's center is reported. Create path: LastFrequency or 14.225 MHz.
inline Decision decide(const Inputs& in)
{
    Decision d;
    d.mode = in.lastMode.isEmpty() ? QStringLiteral("USB") : in.lastMode;
    d.antenna = QStringLiteral("ANT1");

    if (in.hasRestoredPan) {
        d.action = Action::ReuseRestoredPan;
        if (in.restoredPanCenterMhz > 0.0) {
            d.freqMhz = in.restoredPanCenterMhz;
        } else if (in.lastFreqMhz > 0.0) {
            d.freqMhz = in.lastFreqMhz;
        } else {
            d.freqMhz = 14.225000;
        }
        return d;
    }

    d.action = Action::CreateNewPan;
    d.freqMhz = in.lastFreqMhz > 0.0 ? in.lastFreqMhz : 14.225000;
    return d;
}

}  // namespace AetherSDR::SliceRecreatePolicy
