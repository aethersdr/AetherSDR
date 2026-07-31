#pragma once

// Tune-driven pan recenter write policy.
//
// While a KiwiSDR virtual antenna owns a pan's display, the radio-side pan
// geometry is deliberately frozen: zoom/center gestures stay widget-local and
// never advance PanadapterModel (#3825, #4081). That freeze makes the model's
// bandwidth stale the moment the operator zooms — so a center-ONLY write
// through RadioModel::requestPanCenter() re-broadcasts the frozen bandwidth
// via PanadapterModel::infoChanged, and the widget visibly snaps back to the
// assignment-time span mid-gesture. The tune-driven recenter paths (slice-drag
// edge pan and reveal/pan-follow) pick their write mode through
// recenterWrite(); centerActiveSliceInPanadapter keeps its own
// forceRadioCenter gating and consumes only the bandwidth-pairing rule:
//
//  - Flex-display pan → write through the radio and model; the widget
//    repaints via the optimistic model write (the pre-existing path).
//  - Kiwi-display pan → recenter the widget directly, pairing the new center
//    with the WIDGET's live bandwidth; radio and model stay untouched.
//  - Kiwi-display pan during an edge-pan slice drag → no write at all: the
//    SpectrumWidget already advanced its own view, and there is no radio-side
//    span to catch up.
//
// Radio-side recenters that exist for the radio's sake — the WFM DAX-IQ
// center (the NCO rides the pan center) and the ATU pre-tune save/restore
// (center+width written as a pair) — must keep reaching the radio and stay
// outside this policy. Their model writes can still stomp a kiwi-display
// widget's zoom through the infoChanged delivery (as can any center-only
// model write, e.g. the automation bridge's pan-center verb); closing that
// residual class means gating the display-side delivery, a separate concern
// from these tune-path write modes. Precedent for the split applied here:
// snapCenterLockForSlice() since #4116. When a pan LEAVES kiwi display, the
// frozen radio geometry is reconciled one-shot to the widget's view —
// reconcileFlexPanGeometryAfterKiwiDisplay().

namespace AetherSDR::PanRecenterPolicy {

enum class Write {
    RadioAndModel,   // RadioModel::requestPanCenter(): wire command + model
    WidgetLocal,     // SpectrumWidget::setFrequencyRange(center, widget bw)
    None,            // the widget already shows the target view
};

constexpr Write recenterWrite(bool kiwiDisplayActive,
                              bool widgetOwnsViewDuringGesture)
{
    if (!kiwiDisplayActive) {
        return Write::RadioAndModel;
    }
    return widgetOwnsViewDuringGesture ? Write::None : Write::WidgetLocal;
}

// The only bandwidth a recenter may pair with its new center: the widget's
// live span when the kiwi display owns the view (the model's is frozen at
// kiwi-assignment time), the model's otherwise. An uninitialized widget span
// falls back to the model value — matching snapCenterLockForSlice (#4116).
constexpr double recenterBandwidthMhz(bool kiwiDisplayActive,
                                      double widgetBandwidthMhz,
                                      double modelBandwidthMhz)
{
    if (kiwiDisplayActive && widgetBandwidthMhz > 0.0) {
        return widgetBandwidthMhz;
    }
    return modelBandwidthMhz;
}

} // namespace AetherSDR::PanRecenterPolicy
