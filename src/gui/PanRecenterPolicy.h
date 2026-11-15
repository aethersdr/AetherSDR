#pragma once

// Tune-driven pan recenter write policy. While a KiwiSDR owns a pan's display
// the radio-side geometry is frozen (#3825, #4081), so a center-only model
// write re-broadcasts the stale bandwidth and snaps the widget's zoom back.
// recenterWrite() picks the mode for slice-drag edge pan and reveal/pan-follow:
//  - Flex display: write through radio and model.
//  - Kiwi display: recenter the widget with its live bandwidth; no radio write.
//  - Kiwi display during an edge-pan drag: no write (widget already moved).
// Radio-serving recenters (WFM DAX-IQ, ATU pre-tune) stay outside this policy;
// leaving Kiwi display reconciles via reconcileFlexPanGeometryAfterKiwiDisplay().

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
