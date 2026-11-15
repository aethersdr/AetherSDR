#include "core/AetherClockSettings.h"

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QLatin1String>
#include <QString>

#include <cmath>
#include <limits>

namespace AetherSDR {

namespace {
const QString kAetherClockKey = QStringLiteral("AetherClock");

// WWV carrier presets mirrored from AetherClockEngine::wwvCarrierFrequenciesMHz
// (2.5/5/10/15/20 MHz). Hardcoded here to keep the settings unit
// dependency-light — no engine include. per WS-2 authoring spec, Task C.
constexpr double kWwvCarrierPresetsMHz[] = {2.5, 5.0, 10.0, 15.0, 20.0};
constexpr double kDefaultWwvCarrierMHz = 10.0;
} // namespace

// One nested JSON blob under the single AppSettings key "AetherClock"
// (the AprsSettings pattern). Values in the store are strings; the blob
// itself is compact JSON.
QJsonObject AetherClockSettings::readObj()
{
    const QString json =
        AppSettings::instance().value(kAetherClockKey, QString{}).toString();
    if (json.isEmpty())
        return {};
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

void AetherClockSettings::write(const QJsonObject& o)
{
    auto& s = AppSettings::instance();
    s.setValue(kAetherClockKey,
               QString::fromUtf8(
                   QJsonDocument(o).toJson(QJsonDocument::Compact)));
    s.save();
}

QString AetherClockSettings::stationPreset()
{
    return readObj().value(QStringLiteral("stationPreset"))
        .toString(QStringLiteral("WWV"));
}

void AetherClockSettings::setStationPreset(const QString& preset)
{
    // Validate to {"WWV","WWVB"}; anything else falls back to "WWV".
    const QString valid = (preset == QLatin1String("WWVB"))
                              ? QStringLiteral("WWVB")
                              : QStringLiteral("WWV");
    QJsonObject o = readObj();
    o[QStringLiteral("stationPreset")] = valid;
    write(o);
}

double AetherClockSettings::wwvCarrierMHz()
{
    return readObj().value(QStringLiteral("wwvCarrierMHz"))
        .toDouble(kDefaultWwvCarrierMHz);
}

void AetherClockSettings::setWwvCarrierMHz(double mhz)
{
    // Clamp to the nearest legal preset; nonsense (NaN/inf) → default 10.0.
    double chosen = kDefaultWwvCarrierMHz;
    if (std::isfinite(mhz)) {
        double best = std::numeric_limits<double>::infinity();
        for (const double preset : kWwvCarrierPresetsMHz) {
            const double d = std::abs(preset - mhz);
            if (d < best) {
                best = d;
                chosen = preset;
            }
        }
    }
    QJsonObject o = readObj();
    o[QStringLiteral("wwvCarrierMHz")] = chosen; // stored as a JSON number
    write(o);
}

bool AetherClockSettings::debugLogVisible()
{
    return readObj().value(QStringLiteral("debugLogVisible")).toBool(false);
}

void AetherClockSettings::setDebugLogVisible(bool visible)
{
    QJsonObject o = readObj();
    o[QStringLiteral("debugLogVisible")] = visible;
    write(o);
}

} // namespace AetherSDR
