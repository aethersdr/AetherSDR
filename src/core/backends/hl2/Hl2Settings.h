#pragma once

class QJsonObject;

namespace AetherSDR {

// Owned configuration for the Hermes-Lite 2 backend: one nested JSON object
// under the root key "Hl2", read and written atomically, one place to default.
//   spanMhz — the panadapter span the operator last chose, in MHz.
//   cwEdgeLeadMs — the delay from a keyer edge's instant to its EP2 sample.
// The span is the DDC rate: widest costs ~8x narrowest (25.2 vs 3.1 Mbps UDP),
// so first run takes the cheap default and the wide view is opted into once.
class Hl2Settings {
public:
    // The operator's remembered span in MHz, or 0.0 when they have never
    // chosen one — distinct from any real span, so "never set" stays
    // distinguishable from a valid value and the backend can apply its own
    // conservative default.
    static double spanMhz();
    static void setSpanMhz(double mhz);

    // The operator's "Use low bandwidth mode" choice, from the connection
    // panel. Read-only here: a grandfathered flat key ("LowBandwidthConnect")
    // owned by the connection UI; it caps the widest span offered.
    static bool lowBandwidth();

    // The CW edge lead (#5065, #7245): how long after its instant a keyer
    // edge reaches the EP2 stream, so a GUI or I/O-thread stall shorter than
    // it does not change dit, dah or space lengths. RF follows the sidetone by
    // at least this much. Of this computer, not of a radio: it is set by how
    // late this host's event loops deliver an edge, so it is one value for
    // every HL2 here. 0 is safe: an edge later than its sample shifts the rest
    // of the over (MetisClient::setCwKeyDown), lengths still exact, so the
    // RF delay becomes the worst lateness of that over. 100 keeps the lead
    // plus the largest shift inside the edge schedule (MetisClient's
    // static_assert). The default covers the GUI hop measured while keyed on a
    // slow host.
    static constexpr int kCwEdgeLeadMinMs = 0;
    static constexpr int kCwEdgeLeadMaxMs = 100;
    static constexpr int kCwEdgeLeadDefaultMs = 40;
    static constexpr int clampCwEdgeLeadMs(int ms) noexcept
    {
        return ms < kCwEdgeLeadMinMs ? kCwEdgeLeadMinMs
                                     : (ms > kCwEdgeLeadMaxMs ? kCwEdgeLeadMaxMs : ms);
    }
    // kCwEdgeLeadDefaultMs when absent or unparseable; a stored value is
    // clamped (Principle VII).
    static int cwEdgeLeadMs();
    // Clamps, then persists; returns what was stored.
    static int setCwEdgeLeadMs(int ms);


private:
    static QJsonObject readObj();
};

}  // namespace AetherSDR
