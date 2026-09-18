#pragma once

#include <QString>

class QJsonObject;

namespace AetherSDR::anan {

// Owned configuration for the ANAN-G2 backend, per Constitution Principle V:
// one nested JSON object under a single root key ("Anan"), read and written
// atomically, with one place to default. Mirrors IcomSettings's shape
// (02-working-plan.md Step 2: "AnanSettings in params, namespaced anan.*,
// mirroring IcomSettings") rather than the ad-hoc AppSettings::instance().
// value() calls ConnectionPanel/RadioModel used before this existed.
class AnanSettings {
public:
    // DDC0 sample rate in ksps -- also the panadapter span. One of the six
    // rates AnanBackend::capabilities().sampleRatesHz declares. Not
    // validated here; AnanBackend::nearestDdc0RateKsps() is the actual
    // snap-to-valid-rate authority, so an out-of-range or hand-edited value
    // just gets snapped on the next connect/zoom rather than rejected here.
    static int ddc0RateKsps();
    static void setDdc0RateKsps(int ksps);

    // ADC options -- all connect-time-only (no live setter anywhere below
    // the seam; P2Client::Params carries these once, at start()). Spec
    // citations live on P2Protocol::buildDdcSpecific()/buildHighPriority(),
    // the actual wire encoders; this class only owns persistence.
    static bool ditherEnabled();
    static void setDitherEnabled(bool on);
    static bool randomEnabled();
    static void setRandomEnabled(bool on);
    // 0 = ADC0, behind the switched Ant1/2/3 relay bank; 1 = ADC1, wired
    // directly to its own RX2 jack (Appendix D block diagram, spec p.90).
    static int ddc0AdcIndex();
    static void setDdc0AdcIndex(int index);
    static bool bypassAdc0Filters();
    static void setBypassAdc0Filters(bool on);
    static bool bypassAdc1Filters();
    static void setBypassAdc1Filters(bool on);

    // Restore every field to its default.
    static void reset();

private:
    static QJsonObject readObj();
    static void writeObj(const QJsonObject& obj);
};

}  // namespace AetherSDR::anan
