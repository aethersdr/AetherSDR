#include "core/backends/hl2/Hl2FreqCal.h"

#include "core/RadioSettingsScope.h"

#include <QJsonObject>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

double Hl2FreqCal::scaleForPpb(int ppb) noexcept
{
    return 1.0 / (1.0 + static_cast<double>(clampPpb(ppb)) / 1.0e9);
}

int Hl2FreqCal::clampPpb(int ppb) noexcept
{
    return std::clamp(ppb, kMinPpb, kMaxPpb);
}

int Hl2FreqCal::ppbFromZeroBeat(double referenceHz, double dialledHz) noexcept
{
    if (!(referenceHz > 0.0) || !(dialledHz > 0.0))
        return 0;
    // 1 + e = reference / dialled  =>  e = reference/dialled - 1.
    const double e = referenceHz / dialledHz - 1.0;
    if (!std::isfinite(e))
        return 0;
    return clampPpb(static_cast<int>(std::llround(e * 1.0e9)));
}

double Hl2FreqCal::effectiveClockHz(int ppb) noexcept
{
    return kNominalClockHz * (1.0 + static_cast<double>(clampPpb(ppb)) / 1.0e9);
}

double Hl2FreqCal::errorHzAt(double rfHz, int ppb) noexcept
{
    // Uncorrected, a signal at rfHz reads as rfHz/(1+e); the operator sees an
    // error of (that - rfHz). Negative when the clock is fast, matching "a fast
    // clock makes signals appear low".
    return rfHz * (scaleForPpb(ppb) - 1.0);
}

std::uint32_t Hl2FreqCal::ncoCommandHz(double trueHz, double scale) noexcept
{
    const double cmd = trueHz * scale;
    if (!(cmd > 0.0))
        return 0;
    // The register is 32 bits of Hz. The HL2 tunes to 38.4 MHz so this can only
    // be reached by a bad caller, but saturate rather than wrap: a wrapped NCO
    // would tune somewhere plausible-looking and silently wrong.
    if (cmd >= 4'294'967'295.0)
        return 4'294'967'295u;
    return static_cast<std::uint32_t>(std::llround(cmd));
}

double Hl2FreqCal::dspShiftHz(double sliceTrueHz, std::uint32_t ncoCommand,
                              double scale) noexcept
{
    return sliceTrueHz * scale - static_cast<double>(ncoCommand);
}

int Hl2FreqCal::loadPpb(const RadioSettingsScope& scope)
{
    if (!scope.isValid())
        return 0;
    const QJsonObject doc = scope.feature(QLatin1String(kFeature));
    const QJsonValue v = doc.value(QLatin1String(kFieldPpb));
    if (!v.isDouble())
        return 0;
    return clampPpb(static_cast<int>(std::llround(v.toDouble(0.0))));
}

void Hl2FreqCal::savePpb(const RadioSettingsScope& scope, int ppb)
{
    if (!scope.isValid())
        return;
    const int clamped = clampPpb(ppb);
    if (clamped == 0) {
        // Zero is "uncalibrated", which is the absent-document state. Remove the
        // row rather than storing a 0, so a reset genuinely returns the radio to
        // never-calibrated instead of leaving a document that reads the same but
        // shadows any future family-wide default.
        // Check the write result: setFeature()/removeFeature() refuse while the
        // store is not ReadyToSave, and a mutation that silently does not
        // persist while the UI repaints from the spinbox is the worst failure
        // shape here — nothing reads an NCO register back, so the operator would
        // only discover it on the next connect (AGENTS.md, PR #4621 review).
        if (!scope.removeFeature(QLatin1String(kFeature)))
            qWarning("Hl2FreqCal: calibration reset did not persist");
        AppSettings::instance().save();
        return;
    }
    // Read-modify-write so a future field in this document is not dropped by a
    // calibration change (Principle XIV — persisted as a unit).
    QJsonObject doc = scope.featureExact(QLatin1String(kFeature));
    doc[QLatin1String(kFieldPpb)] = clamped;
    if (!scope.setFeature(QLatin1String(kFeature), kSchemaVersion, doc))
        qWarning("Hl2FreqCal: %d ppb did not persist", clamped);
    AppSettings::instance().save();
}

}  // namespace AetherSDR
