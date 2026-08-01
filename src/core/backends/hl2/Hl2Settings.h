#pragma once

class QJsonObject;

namespace AetherSDR {

// Owned configuration for the Hermes-Lite 2 backend, per Constitution
// Principle V: one nested JSON object under a single root key ("Hl2"),
// read and written atomically, with one place to default.
//
//   spanMhz — the panadapter span the operator last chose, in MHz.
//
// WHY THE SPAN IS PERSISTED. On this radio the pan span IS the DDC sample
// rate, so it is not a free display choice: the widest span costs ~8x the
// narrowest in network bandwidth (25.2 vs 3.1 Mbps sustained UDP, 3048 vs 381
// packets/second) and 8x the samples through WDSP's decimation front end.
//
// That rules out defaulting to the widest — it would impose the cost on every
// operator at connect, including on wifi and on hosts that cannot carry it.
// But defaulting to the narrowest with no memory is the bug this whole change
// exists to fix: the operator would be back to a 48 kHz window on every launch.
//
// Persisting resolves both. First run is the cheap default; an operator who
// wants the wide view chooses it once and keeps it, and the cost is opted into
// rather than imposed.
class Hl2Settings {
public:
    // The operator's remembered span in MHz, or 0.0 when they have never
    // chosen one — distinct from any real span, so "never set" stays
    // distinguishable from a valid value and the backend can apply its own
    // conservative default.
    static double spanMhz();
    static void setSpanMhz(double mhz);

    // The operator's "Use low bandwidth mode" choice, from the connection
    // panel. READ ONLY from here: it is a grandfathered flat key
    // ("LowBandwidthConnect") owned by the connection UI, and Principle V
    // forbids adding to the flat namespace — this backend consumes it to cap
    // the widest span it will offer, and never writes it.
    static bool lowBandwidth();


private:
    static QJsonObject readObj();
};

}  // namespace AetherSDR
