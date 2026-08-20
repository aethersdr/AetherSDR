#include "XvtrPolicy.h"

#include <QMap>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace AetherSDR::XvtrPolicy {

namespace {

constexpr double kMaxPowerFloorDbm = -10.0;
constexpr double kHighIfThresholdMhz = 80.0;
constexpr double kHighIfMaxPowerDbm = 8.0;
constexpr double kLowIfLegacyMaxPowerDbm = 10.0;
constexpr double kLowIfDefaultMaxPowerDbm = 15.0;

bool isNativeBandKey(const QString& key, ModelCapabilities caps)
{
    static const QSet<QString> kAlwaysNativeBandKeys = {
        QStringLiteral("160"), QStringLiteral("80"), QStringLiteral("60"),
        QStringLiteral("40"),  QStringLiteral("30"), QStringLiteral("20"),
        QStringLiteral("17"),  QStringLiteral("15"), QStringLiteral("12"),
        QStringLiteral("10"),  QStringLiteral("6"),
        QStringLiteral("2200"), QStringLiteral("630")
    };
    if (kAlwaysNativeBandKeys.contains(key))
        return true;
    if (caps.has4Meters && key == QLatin1String("4"))
        return true;
    if (caps.has2Meters && key == QLatin1String("2"))
        return true;
    return false;
}

QString normalizedNativeBandKey(const QString& bandName, ModelCapabilities caps)
{
    QString key = bandName;
    if (key.endsWith('m') && key.length() > 1) {
        const QString stripped = key.chopped(1);
        if (isNativeBandKey(stripped, caps))
            key = stripped;
    }
    return key;
}

double tileBandwidth(double lowMhz, double highMhz)
{
    return highMhz - lowMhz;
}

double tileCenter(double lowMhz, double highMhz)
{
    return (lowMhz + highMhz) / 2.0;
}

bool usesLegacyLowIfMaxPower(const QString& radioModel)
{
    const QString model = radioModel.trimmed().toUpper();
    return model == QLatin1String("FLEX-6400") ||
           model == QLatin1String("FLEX-6400M") ||
           model == QLatin1String("FLEX-6600") ||
           model == QLatin1String("FLEX-6600M");
}

} // namespace

BandStackKeyResult resolveBandStackKey(const QString& bandName,
                                       const QVector<Transverter>& xvtrs,
                                       ModelCapabilities caps)
{
    static const QMap<QString, int> kNumericBandSlots = {
        { QStringLiteral("WWV"), 33 },
        { QStringLiteral("GEN"), 34 },
    };

    const QString radioKey = normalizedNativeBandKey(bandName, caps);
    if (isNativeBandKey(radioKey, caps))
        return {radioKey, {}};

    if (kNumericBandSlots.contains(bandName))
        return {QString::number(kNumericBandSlots.value(bandName)), {}};

    for (const auto& xvtr : xvtrs) {
        if (!xvtr.isValid || xvtr.name != bandName)
            continue;

        return {QString("X%1").arg(xvtr.index), {}};
    }

    return {
        {},
        QString("Band %1 has no Flex display pan band= mapping").arg(bandName)
    };
}

BandTuneAdmissibility evaluateBandTune(bool usesFlexCommandPlane,
                                       const QString& bandName,
                                       double targetMhz,
                                       double tuningMinHz,
                                       double tuningMaxHz,
                                       const QVector<Transverter>& xvtrs,
                                       ModelCapabilities caps)
{
    if (!usesFlexCommandPlane) {
        // No band stack to preselect, so the only honest question is whether
        // the receiver reaches the frequency at all — the same question the
        // band buttons already ask, and now through this same function (#5041).
        const double hz = targetMhz * 1.0e6;
        const double minMhz = tuningMinHz / 1.0e6;
        const double maxMhz = tuningMaxHz / 1.0e6;
        if (tuningMaxHz > tuningMinHz && (hz < tuningMinHz || hz > tuningMaxHz)) {
            BandTuneAdmissibility refused;
            // The LOG form: the numbers that decided it, in the units the
            // caller passed. The operator's sentence is composed once, in
            // bandTuneRefusalText(), from the typed fields below.
            refused.reason =
                QString("band %1 at %2 MHz is outside the backend's declared "
                        "tuning range %3-%4 MHz")
                    .arg(bandName)
                    .arg(targetMhz, 0, 'f', 6)
                    .arg(minMhz, 0, 'f', 3)
                    .arg(maxMhz, 0, 'f', 3);
            refused.outsideTuningRange = true;
            refused.rangeMinMhz = minMhz;
            refused.rangeMaxMhz = maxMhz;
            return refused;
        }
        BandTuneAdmissibility admitted;
        admitted.supported = true;
        return admitted;
    }

    const auto stackKeyResult = resolveBandStackKey(bandName, xvtrs, caps);
    if (stackKeyResult.isSupported()) {
        BandTuneAdmissibility admitted;
        admitted.supported = true;
        admitted.bandStackKey = stackKeyResult.key;
        return admitted;
    }

    BandTuneAdmissibility refused;
    refused.reason = stackKeyResult.unsupportedReason;
    if (targetMhz > 54.0 && xvtrs.isEmpty()) {
        refused.reason =
            QString("Band %1 requires a configured XVTR before Aether can tune it.")
                .arg(bandName);
    }
    return refused;
}

bool isWaterfallTileOutsidePan(double lowMhz, double highMhz, double panCenterMhz)
{
    const double bw = tileBandwidth(lowMhz, highMhz);
    if (bw <= 0.0)
        return false;

    return std::abs(tileCenter(lowMhz, highMhz) - panCenterMhz) > bw;
}

WaterfallTileMatch matchWaterfallTileTransverterOffset(double lowMhz, double highMhz,
                                                       double panCenterMhz,
                                                       const QVector<Transverter>& xvtrs)
{
    WaterfallTileMatch match;
    const double bw = tileBandwidth(lowMhz, highMhz);
    if (bw <= 0.0 || !isWaterfallTileOutsidePan(lowMhz, highMhz, panCenterMhz))
        return match;

    match.observedOffsetMhz = panCenterMhz - tileCenter(lowMhz, highMhz);
    match.toleranceMhz = std::max(bw, 0.25);
    for (const auto& xvtr : xvtrs) {
        if (!xvtr.isValid || xvtr.rfFreqMhz <= 0.0 || xvtr.ifFreqMhz <= 0.0)
            continue;

        const double expectedOffset = xvtr.rfFreqMhz - xvtr.ifFreqMhz;
        if (std::abs(match.observedOffsetMhz - expectedOffset) <= match.toleranceMhz) {
            match.matched = true;
            match.index = xvtr.index;
            match.order = xvtr.order;
            match.name = xvtr.name;
            match.expectedOffsetMhz = expectedOffset;
            return match;
        }
    }

    return match;
}

bool waterfallTileMatchesTransverterOffset(double lowMhz, double highMhz,
                                           double panCenterMhz,
                                           const QVector<Transverter>& xvtrs)
{
    return matchWaterfallTileTransverterOffset(
        lowMhz, highMhz, panCenterMhz, xvtrs).matched;
}

WaterfallTileRange mapWaterfallTileRange(double lowMhz, double highMhz,
                                         double panCenterMhz,
                                         const QVector<Transverter>& xvtrs,
                                         bool hasXvtrSliceAntenna)
{
    if (!isWaterfallTileOutsidePan(lowMhz, highMhz, panCenterMhz))
        return {lowMhz, highMhz, false};

    if (!hasXvtrSliceAntenna &&
        !waterfallTileMatchesTransverterOffset(lowMhz, highMhz, panCenterMhz, xvtrs)) {
        return {lowMhz, highMhz, false};
    }

    const double offset = panCenterMhz - tileCenter(lowMhz, highMhz);
    return {lowMhz + offset, highMhz + offset, true};
}

MaxPowerRange maxPowerRangeFor(double ifFreqMhz, const QString& radioModel)
{
    if (ifFreqMhz >= kHighIfThresholdMhz) {
        return {kMaxPowerFloorDbm, kHighIfMaxPowerDbm};
    }

    if (usesLegacyLowIfMaxPower(radioModel)) {
        return {kMaxPowerFloorDbm, kLowIfLegacyMaxPowerDbm};
    }

    return {kMaxPowerFloorDbm, kLowIfDefaultMaxPowerDbm};
}

double clampMaxPowerDbm(double maxPowerDbm, double ifFreqMhz, const QString& radioModel)
{
    const MaxPowerRange range = maxPowerRangeFor(ifFreqMhz, radioModel);
    return std::clamp(maxPowerDbm, range.minimumDbm, range.maximumDbm);
}

} // namespace AetherSDR::XvtrPolicy
