#include "core/backends/anan/AnanSettings.h"

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace AetherSDR::anan {
namespace {

// Single nested-JSON key holding this backend's config (Principle V).
// Shape: {"ddc0RateKsps":int, "dither":bool,
//         "random":bool, "ddc0AdcIndex":int, "bypassAdc0Filters":bool,
//         "bypassAdc1Filters":bool}
const QString kRootKey = QStringLiteral("Anan");

constexpr const char* kFieldDdc0RateKsps      = "ddc0RateKsps";
constexpr const char* kFieldDither            = "dither";
constexpr const char* kFieldRandom            = "random";
constexpr const char* kFieldDdc0AdcIndex      = "ddc0AdcIndex";
constexpr const char* kFieldBypassAdc0Filters = "bypassAdc0Filters";
constexpr const char* kFieldBypassAdc1Filters = "bypassAdc1Filters";

}  // namespace

QJsonObject AnanSettings::readObj()
{
    const QString json = AppSettings::instance().value(kRootKey, QString{}).toString();
    if (json.isEmpty())
        return {};
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

void AnanSettings::writeObj(const QJsonObject& obj)
{
    auto& s = AppSettings::instance();
    s.setValue(kRootKey,
               QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    // COMMIT -- setValue() only marks the row dirty; save() runs the
    // transaction and there is no autosave timer. See IcomSettings::
    // writeObj(), the sibling this is modelled on, for the same note.
    s.save();
}

int AnanSettings::ddc0RateKsps()
{
    const int v = readObj().value(QLatin1String(kFieldDdc0RateKsps)).toInt(48);
    return v > 0 ? v : 48;
}

void AnanSettings::setDdc0RateKsps(int ksps)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldDdc0RateKsps)] = ksps;
    writeObj(obj);
}

bool AnanSettings::ditherEnabled()
{
    return readObj().value(QLatin1String(kFieldDither)).toBool(true);
}

void AnanSettings::setDitherEnabled(bool on)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldDither)] = on;
    writeObj(obj);
}

bool AnanSettings::randomEnabled()
{
    return readObj().value(QLatin1String(kFieldRandom)).toBool(true);
}

void AnanSettings::setRandomEnabled(bool on)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldRandom)] = on;
    writeObj(obj);
}

int AnanSettings::ddc0AdcIndex()
{
    const int v = readObj().value(QLatin1String(kFieldDdc0AdcIndex)).toInt(0);
    return v == 1 ? 1 : 0;   // anything but a deliberate 1 reads as ADC0
}

void AnanSettings::setDdc0AdcIndex(int index)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldDdc0AdcIndex)] = (index == 1 ? 1 : 0);
    writeObj(obj);
}

bool AnanSettings::bypassAdc0Filters()
{
    return readObj().value(QLatin1String(kFieldBypassAdc0Filters)).toBool(true);
}

void AnanSettings::setBypassAdc0Filters(bool on)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldBypassAdc0Filters)] = on;
    writeObj(obj);
}

bool AnanSettings::bypassAdc1Filters()
{
    return readObj().value(QLatin1String(kFieldBypassAdc1Filters)).toBool(true);
}

void AnanSettings::setBypassAdc1Filters(bool on)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldBypassAdc1Filters)] = on;
    writeObj(obj);
}

void AnanSettings::reset()
{
    writeObj({});
}

}  // namespace AetherSDR::anan
