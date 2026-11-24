#include "core/TxOffDelaySettings.h"

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonValue>
#include <QString>

#include <algorithm>

namespace AetherSDR {

namespace {
const QString kTxOffDelayKey = QStringLiteral("TxOffDelay");
const QString kMsField       = QStringLiteral("ms");
} // namespace

QJsonObject TxOffDelaySettings::readObj()
{
    const QString json =
        AppSettings::instance().value(kTxOffDelayKey, QString{}).toString();
    if (json.isEmpty()) {
        return {};
    }
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

void TxOffDelaySettings::write(const QJsonObject& o)
{
    auto& s = AppSettings::instance();
    s.setValue(kTxOffDelayKey,
               QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    s.save();
}

int TxOffDelaySettings::clampMs(int ms)
{
    return std::clamp(ms, kMinMs, kMaxMs);
}

int TxOffDelaySettings::delayMs()
{
    const QJsonValue v = readObj().value(kMsField);
    if (!v.isDouble()) {
        return 0;
    }
    const double raw = v.toDouble();
    // Out-of-range is treated as corrupt, not clamped: a hand-edited 50000
    // must not silently become a multi-second hold on the operator's signal.
    if (!(raw >= kMinMs && raw <= kMaxMs)) {
        return 0;
    }
    return static_cast<int>(raw);
}

void TxOffDelaySettings::setDelayMs(int ms)
{
    QJsonObject o = readObj();
    o[kMsField] = clampMs(ms);
    write(o);
}

} // namespace AetherSDR
