#pragma once

namespace AetherSDR {

// MainWindow reaches the active-slice setter from three radio-driven sources:
// an explicit active=1 status, a topology fallback while slices are being
// removed/recreated, and the connect-time bootstrap of the first enumerated
// slice. Outside a band recall each retains its established behavior. During a
// FLEX band-stack rebuild, none may reveal the transient slice or send active=1
// back: any of these writes can race the radio-authoritative pan/slice
// reconstruction and undo the selected band.
enum class RadioSliceSelectionSource {
    ActiveStatus,
    TopologyFallback,
    // The first slice to arrive during connect enumeration when the client has no
    // active slice, selected only so the UI has a target before the enumeration
    // finishes. It is arrival ORDER, not the operator's choice — slice 0 is simply
    // enumerated first — so it must never write active=1 back. During connect the
    // status burst / enumeration order can leave a different slice active (often
    // the last one created). Asserting a selection here overwrites that live
    // status before later slices have even been created client-side, so the
    // first-enumerated slice always won and the operator's slice B silently became
    // slice A on every launch.
    InitialEnumeration,
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

    // TopologyFallback is the only source that may assert the selection: it
    // fires when the active slice was removed or created into an empty list
    // mid-session, so the radio has no valid active slice and must be told
    // which one takes over.
    return {
        true,
        source != RadioSliceSelectionSource::TopologyFallback,
    };
}

inline RadioSliceSelectionSource firstSliceSelectionSource(bool initialConnectEnumeration)
{
    return initialConnectEnumeration
        ? RadioSliceSelectionSource::InitialEnumeration
        : RadioSliceSelectionSource::TopologyFallback;
}

inline const char* radioSliceSelectionSourceName(RadioSliceSelectionSource source)
{
    switch (source) {
    case RadioSliceSelectionSource::ActiveStatus:
        return "active-status";
    case RadioSliceSelectionSource::TopologyFallback:
        return "topology-fallback";
    case RadioSliceSelectionSource::InitialEnumeration:
        return "initial-enumeration";
    }
    return "unknown";
}

}  // namespace AetherSDR
