#pragma once

#include <QLatin1String>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <array>

// THE PRODUCER→CONSUMER JOIN for meters.
//
// A meter crosses three boundaries before an operator sees it, and until now
// nothing in the codebase knew about more than one of them at a time:
//
//   1. the radio's transport publishes it   (backend → meterUpdate "SRC:NAME")
//   2. MeterModel routes and CONVERTS it    (by name, applying a unit)
//   3. one or more applets render it        (via a typed accessor or signal)
//
// Every meter bug this project has chased lived in a gap between two of those.
// A meter defined but never fed is 1→2. A meter fed in watts and converted as
// dBm is 2's assumption disagreeing with 1's declaration — it renders motionless
// while every check at boundary 1 reports it healthy. A meter converted
// correctly and read by no applet at all is 2→3, and is completely invisible:
// nothing is wrong anywhere you would think to look.
//
// This table is the join, written down. It is the only place that knows a
// meter's whole path, which makes it the only place that can answer "will the
// operator see this", and it lets `radiocert` and the bridge report a gap
// instead of leaving it to be found on the air.
//
// IT IS HAND-MAINTAINED, and that is a real cost worth naming. There is no way
// to derive "which applet reads this" from the code — the link is a signal
// connection, not a declaration. The mitigation is that the checks built on it
// are two-directional: a meter with no surface listed and a surface whose
// accessor never receives are both reportable, so the table rotting shows up as
// a finding rather than as silence.

namespace AetherSDR {

struct MeterSurface {
    // "SRC:NAME" exactly as the backend publishes it over the seam.
    const char* key;

    // Every unit the consumer can correctly handle, comma-separated.
    //
    // A SET, not a single value, and the difference matters. The first version
    // of this field held the one unit the consumer applied, and running it
    // against a live radio flagged both FWDPWR and ALC as mismatched — after
    // they had been fixed. The fix was precisely to make those consumers honour
    // the DECLARED unit, so equality against a single expected value now
    // reports a permanent false positive on a healthy meter. Which is §1.28's
    // warning: a concern that never goes away stops being read, and takes the
    // real ones with it.
    //
    // The question worth asking is "can the consumer handle what the backend
    // declared", so the answer is set membership. A backend declaring "mW" is
    // still flagged, which is the case this exists for.
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
    {"SLC:LEVEL", "dBm", "MeterModel::sLevelChanged / sLevel()",
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

    // THE GAIN, not the level — the other half of the same stage, and a
    // separate quantity rather than a second view of TX:ALC. TX:ALC is the
    // post-ALC peak, which `Hl2TxDsp::processAudioBlock` describes as a meter
    // that "sits pinned near the target by definition and tells the operator
    // nothing — it reports the ALC's success, not their input level." This is
    // the number that answers "is the ALC holding, and by how much". This PR
    // publishes that value through MeterModel and the diagnostic surfaces;
    // the operator-facing gauge is deliberately a separate change (#5636).
    //
    // dB ONLY, deliberately, where TX:ALC accepts dBFS or Percent. That set has
    // two members because an Icom reports its ALC level as a percentage of its
    // own full scale; nothing in the tree reports a GAIN as anything but dB, so
    // a second accepted unit here would be a conversion nobody can perform
    // correctly rather than a radio this consumer can handle. WDSP draws the
    // same line — `TXA_ALC_PK` and `TXA_ALC_GAIN` are separate entries in
    // `txaMeterType` in `third_party/wdsp/upstream/TXA.h`.
    {"TX:ALCGAIN", "dB", "MeterModel::alcGainChanged / alcGainDb()",
     "No GUI surface (producer-only; proposed Phone panel gauge is #5636)", false},

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

// Does the consumer understand what the backend declared?
//
// THE one implementation of this question, and it is a function rather than
// three copies of a split-and-compare loop because §1.38 is precisely what
// happens when two places answer it differently. `acceptedUnits` was widened
// from a single value to a set here, the automation `meters` join was updated,
// and `kMeterTable` in RadioCertification.cpp was not — so `radiocert meters`
// reported UNIT MISMATCH on a healthy TX:ALC on every run, and ranked it above
// every real finding. There is now nowhere left to hold the old form.
//
// An empty declaration is NOT a mismatch: a backend that declares no unit has
// made no claim to disagree with, and reporting one would invent a defect out
// of missing metadata. An empty accepted set is not a mismatch either — it
// means nobody has written down what the consumer handles, which is a gap in
// this table and not a fault in the radio.
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
