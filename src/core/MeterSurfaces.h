#pragma once

#include <QLatin1String>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <array>

// The producer->consumer join for meters. A meter crosses three boundaries:
//   1. the transport publishes it   (backend -> meterUpdate "SRC:NAME")
//   2. MeterModel routes and converts it (by name, applying a unit)
//   3. applets render it            (typed accessor or signal)
// Gaps: defined but never fed (1->2), unit mismatch (renders motionless while 1
// looks healthy), converted but read by no applet (2->3). This table is the only
// place that knows the whole path, so `radiocert` and the bridge can report gaps.
// Hand-maintained (applet links are signal connections); the checks run both
// ways (meter without surface, surface never fed) so rot shows up as findings.

namespace AetherSDR {

struct MeterSurface {
    // "SRC:NAME" exactly as the backend publishes it over the seam.
    const char* key;

    // Every unit the consumer handles correctly, comma-separated. A SET: consumers
    // honour the backend's declared unit, so the check is "can the consumer handle
    // what was declared" (set membership); a backend declaring e.g. "mW" is flagged.
    const char* acceptedUnits;

    // The typed accessor an applet reads, or the signal it connects to. This is
    // the thing that actually carries the value the last few inches.
    const char* consumer;

    // Where a human looks to see it. Named as the operator would say it, not as
    // the class is called, because the point of this column is to answer "which
    // gauge is wrong".
    const char* surfaces;

    // False when NO UI surface reads this meter. Not a defect on its own —
    // a backend may publish more than the UI shows — but it must be visible,
    // because "published and rendered nowhere" and "rendered and broken" are
    // indistinguishable to anyone reading a meter inventory.
    bool rendered;
};

// Verified against the wiring, not assumed. Each `surfaces` entry corresponds
// to a connect() found in the GUI; a meter listed here with rendered=false has
// no such connection anywhere.
inline constexpr std::array<MeterSurface, 10> kMeterSurfaces{{
    // sLevelForSlice(), NOT the sLevel() scalar this row used to name. That
    // scalar had no writer, so half of what this row advertised was a dead
    // path while the row itself said the listing was "verified against the
    // wiring, not assumed" (#5499 item 2).
    {"SLC:LEVEL", "dBm", "MeterModel::sLevelChanged / sLevelForSlice()",
     "S-meter applet; VFO slice signal flag; AGC calibration dialog", true},

    {"TX:FWDPWR", "Watts,dBm", "MeterModel::directionalPowerMetersChanged / fwdPowerInstant()",
     "TX Controls power gauge (PEP peak-hold); Health applet", true},

    {"TX:REFPWR", "Watts,dBm", "MeterModel::directionalPowerMetersChanged / reflectedPower()",
     "Health applet; feeds the derived SWR when a radio publishes no native one", true},

    {"TX:SWR", "SWR", "MeterModel::directionalPowerMetersChanged / swr()",
     "TX Controls SWR gauge; Health applet", true},

    // GATED ON FORWARD POWER, and that dependency has bitten once already: a
    // mis-scaled FWDPWR reading zero suppressed a perfectly correct SWR, and
    // nothing in the inventory said so. A suppressed meter must name its gate.
    {"TX:ALC", "dBFS,Percent", "MeterModel::swAlcChanged",
     "Phone/CW applet ALC gauge (both Phone and CW panels)", true},

    // The ALC GAIN, a separate quantity from TX:ALC (the post-ALC peak, which sits
    // at the target by definition). Answers "is the ALC holding, and by how much";
    // the Phone panel renders it when the radio declares the meter (#5636). dB only:
    // TX:ALC also accepts Percent because Icom reports ALC level that way, but no
    // radio reports a gain otherwise. WDSP likewise separates TXA_ALC_PK and
    // TXA_ALC_GAIN (third_party/wdsp/upstream/TXA.h).
    {"TX:ALCGAIN", "dB", "MeterModel::alcGainChanged / alcGainDb()",
     "Phone applet ALC Gain gauge (when the meter is defined)", true},

    {"TX:COMPPEAK", "dB", "MeterModel::compressionChanged",
     "Phone/CW applet Compression gauge", true},

    {"TX:MICPEAK", "dBFS", "MeterModel::micPeak()",
     "Phone/CW applet Level gauge", true},

    {"RAD:PATEMP", "degC", "MeterModel::hwTelemetryChanged / paTemp()",
     "Status bar temperature label; Meter applet", true},

    {"RAD:+13.8A", "Volts", "MeterModel::hwTelemetryChanged / supplyVolts()",
     "Status bar voltage label; Meter applet", true},
}};

// Look up the registered consumer metadata for a meter. The returned row may
// still have rendered=false: MeterModel and diagnostics can consume a value
// before any GUI surface exists for it.
[[nodiscard]] inline const MeterSurface* meterSurfaceFor(QStringView key)
{
    for (const auto& s : kMeterSurfaces) {
        if (key == QLatin1String(s.key)) {
            return &s;
        }
    }
    return nullptr;
}

[[nodiscard]] inline bool meterHasRenderedSurface(QStringView key)
{
    const MeterSurface* surface = meterSurfaceFor(key);
    return surface && surface->rendered;
}

// Does the consumer understand what the backend declared? The one
// implementation; the automation `meters` join and radiocert's kMeterTable both
// use it so they can't disagree. An empty declaration, or an empty accepted set
// (an undocumented consumer), is NOT a mismatch.
[[nodiscard]] inline bool meterUnitAccepted(QStringView acceptedUnits,
                                            QStringView declaredUnit)
{
    if (declaredUnit.isEmpty()) {
        return true;
    }
    const QList<QStringView> accepted =
        acceptedUnits.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (accepted.isEmpty()) {
        return true;
    }
    for (QStringView u : accepted) {
        if (declaredUnit.compare(u.trimmed(), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

}  // namespace AetherSDR
