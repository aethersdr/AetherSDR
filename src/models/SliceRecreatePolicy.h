#pragma once

#include <QString>

// SliceRecreatePolicy — decides how to recover a default slice when the radio
// reports zero slices at GUI-attach time ("slice list" returns empty).
//
// Background (#3212): when AetherSDR reconnects to a radio that remembers our
// persistent client_id, the radio's GUIClientID session restore can bring back
// our panadapter WITHOUT its slice (observed after an unexpected disconnect +
// auto-reconnect). The pan is "claimed" well before the "slice list" query
// resolves, so by decision time we already hold the restored pan. The old code
// unconditionally issued "display panafall create", allocating a SECOND, empty
// panadapter on top of the restored one — the duplicate-PAN bug in #3212.
//
// The decision is pulled out of RadioModel into this pure, header-only function
// so it can be unit-tested without a live radio connection (mirrors the
// RadioStatusOwnership pattern). RadioModel feeds it the runtime state and acts
// on the returned Decision.

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
    // ("display pan ... center="). <= 0 means the radio has not reported a
    // center yet (pan only just claimed).
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

// Pure decision. No I/O, no Qt event loop — safe to unit-test.
//
// Frequency choice rationale:
//   * Reuse path: place the slice at the RESTORED PAN'S OWN center, not at
//     LastFrequency. The radio's restored pan center is authoritative for where
//     that pan is displayed right now; LastFrequency is a client-side guess that
//     can be on a different band entirely (in the #3212 bundle the restored pan
//     was on 20m / 14.282 MHz while LastFrequency was 28.305 MHz / 10m — using
//     LastFrequency would drop the slice ~14 MHz outside the visible span).
//     LastFrequency / 14.225 are fallbacks only for the brief window before the
//     radio has reported the pan's center.
//   * Create path: no pan exists to anchor to, so LastFrequency (or the 14.225
//     default) is the best available starting point.
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
