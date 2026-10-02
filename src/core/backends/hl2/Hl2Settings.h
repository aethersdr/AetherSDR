#pragma once

class QJsonObject;

namespace AetherSDR {

// Owned configuration for the Hermes-Lite 2 backend: one nested JSON object
// under the root key "Hl2", read and written atomically, one place to default.
//   spanMhz — the panadapter span the operator last chose, in MHz.
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


private:
    static QJsonObject readObj();
};

}  // namespace AetherSDR
