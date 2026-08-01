#pragma once

namespace AetherSDR {

// MainWindow reaches the active-slice setter from two radio-driven sources:
// an explicit active=1 status and a topology fallback while slices are being
// removed/recreated. Outside a band recall both retain their established
// behavior. During a FLEX band-stack rebuild, neither may reveal the transient
// slice or send active=1 back: either write can race the radio-authoritative
// pan/slice reconstruction and undo the selected band.
enum class RadioSliceSelectionSource {
    ActiveStatus,
    TopologyFallback,
};

struct RadioSliceSelectionDecision {
    bool revealOffscreen{true};
    bool suppressActiveCommand{false};
};

inline RadioSliceSelectionDecision radioSliceSelectionDecision(
    bool bandRecallInFlight,
    RadioSliceSelectionSource source)
{
    if (bandRecallInFlight) {
        return {false, true};
    }

    return {
        true,
        source == RadioSliceSelectionSource::ActiveStatus,
    };
}

}  // namespace AetherSDR
