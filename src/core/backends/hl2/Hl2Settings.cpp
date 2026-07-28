#include "core/backends/hl2/Hl2Settings.h"

#include "core/AppSettings.h"
#include "core/backends/hl2/MetisProtocol.h"   // kMaxReceivers

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
constexpr const char* kFieldReceiverCount = "receiverCount";

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

int Hl2Settings::receiverCount()
{
    // Clamped on READ as well as on write. A hand-edited or truncated settings
    // file must not be able to command a receiver count the protocol cannot
    // encode, and defaulting to 1 on anything unparseable keeps the failure mode
    // "the radio comes up as it always did" rather than "the radio will not
    // start" (Principle VII).
    const int v = readObj().value(QLatin1String(kFieldReceiverCount)).toInt(1);
    if (v < 1)
        return 1;
    if (v > hl2::kMaxReceivers)
        return hl2::kMaxReceivers;
    return v;
}

void Hl2Settings::setReceiverCount(int count)
{
    if (count < 1 || count > hl2::kMaxReceivers)
        return;
    QJsonObject o = readObj();
    o[QLatin1String(kFieldReceiverCount)] = count;
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
