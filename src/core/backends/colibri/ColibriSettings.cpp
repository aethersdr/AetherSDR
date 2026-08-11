#include "core/backends/colibri/ColibriSettings.h"

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace AetherSDR {

namespace {
// Single nested-JSON key holding this backend's config (Principle V).
// Shape: {"spanMhz":double, "dllPath":string, "wireAnalytic":bool}.
const QString kRootKey = QStringLiteral("Colibri");

constexpr const char* kFieldSpanMhz = "spanMhz";
constexpr const char* kFieldDllPath = "dllPath";
constexpr const char* kFieldWireAnalytic = "wireAnalytic";
}  // namespace

QJsonObject ColibriSettings::readObj()
{
    const QString json =
        AppSettings::instance().value(kRootKey, QString{}).toString();
    if (json.isEmpty())
        return {};
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

void ColibriSettings::writeObj(const QJsonObject& obj)
{
    // Read-modify-write the whole object so it is always persisted as a unit
    // (Principle XIV) — never half a config after a crash mid-write.
    auto& s = AppSettings::instance();
    s.setValue(kRootKey,
               QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    s.save();
}

double ColibriSettings::spanMhz()
{
    // 0.0 both when absent and when unparseable: no remembered span, apply
    // the default. Validating here keeps a hand-edited settings file from
    // commanding a nonsense sample rate (Principle VII).
    const double v = readObj().value(QLatin1String(kFieldSpanMhz)).toDouble(0.0);
    return v > 0.0 ? v : 0.0;
}

void ColibriSettings::setSpanMhz(double mhz)
{
    if (!(mhz > 0.0))
        return;
    QJsonObject o = readObj();
    o[QLatin1String(kFieldSpanMhz)] = mhz;
    writeObj(o);
}

QString ColibriSettings::dllPath()
{
    return readObj().value(QLatin1String(kFieldDllPath)).toString();
}

bool ColibriSettings::wireAnalytic()
{
    return readObj().value(QLatin1String(kFieldWireAnalytic)).toBool(true);
}

void ColibriSettings::setWireAnalytic(bool analytic)
{
    QJsonObject o = readObj();
    o[QLatin1String(kFieldWireAnalytic)] = analytic;
    writeObj(o);
}

}  // namespace AetherSDR
