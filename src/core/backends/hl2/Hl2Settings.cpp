#include "core/backends/hl2/Hl2Settings.h"

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace AetherSDR {

namespace {
// Single nested-JSON key holding this backend's config (Principle V).
// Shape: {"spanMhz":double}. There are no legacy flat keys to migrate — this
// object is new, so it starts in the correct shape rather than being
// grandfathered into it.
const QString kRootKey = QStringLiteral("Hl2");

constexpr const char* kFieldSpanMhz = "spanMhz";

// Owned by the connection panel, not by us. Read only; see the header.
const QString kLowBandwidthKey = QStringLiteral("LowBandwidthConnect");
}  // namespace

QJsonObject Hl2Settings::readObj()
{
    const QString json =
        AppSettings::instance().value(kRootKey, QString{}).toString();
    if (json.isEmpty())
        return {};
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

double Hl2Settings::spanMhz()
{
    // 0.0 both when the key is absent and when it holds something
    // unparseable, which is the same answer: we have no remembered span, so
    // the caller applies its default. Validating here rather than trusting the
    // stored value keeps a hand-edited or truncated settings file from
    // commanding a nonsense DDC rate (Principle VII).
    const double v = readObj().value(QLatin1String(kFieldSpanMhz)).toDouble(0.0);
    return v > 0.0 ? v : 0.0;
}

void Hl2Settings::setSpanMhz(double mhz)
{
    if (!(mhz > 0.0))
        return;
    // Read-modify-write the whole object so it is always persisted as a unit
    // (Principle XIV) — never half a config after a crash mid-write.
    QJsonObject o = readObj();
    o[QLatin1String(kFieldSpanMhz)] = mhz;
    auto& s = AppSettings::instance();
    s.setValue(kRootKey,
               QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    s.save();
}

bool Hl2Settings::lowBandwidth()
{
    return AppSettings::instance()
               .value(kLowBandwidthKey, QStringLiteral("False"))
               .toString()
           == QLatin1String("True");
}

}  // namespace AetherSDR
