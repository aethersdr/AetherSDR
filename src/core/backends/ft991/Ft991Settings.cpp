#include "core/backends/ft991/Ft991Settings.h"

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace AetherSDR {

namespace {
// Single nested-JSON key holding this backend's config (Principle V).
// Shape: {"baudRate":int, "audioInHint":string, "audioOutHint":string}.
const QString kRootKey = QStringLiteral("Ft991");

constexpr const char* kFieldBaudRate = "baudRate";
constexpr const char* kFieldAudioInHint = "audioInHint";
constexpr const char* kFieldAudioOutHint = "audioOutHint";

constexpr int kDefaultBaud = 38400;
}  // namespace

QJsonObject Ft991Settings::readObj()
{
    const QString json =
        AppSettings::instance().value(kRootKey, QString{}).toString();
    if (json.isEmpty())
        return {};
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

void Ft991Settings::writeObj(const QJsonObject& obj)
{
    // Read-modify-write the whole object so it is always persisted as a unit
    // (Principle XIV) — never half a config after a crash mid-write.
    auto& s = AppSettings::instance();
    s.setValue(kRootKey,
               QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    s.save();
}

int Ft991Settings::baudRate()
{
    // Only the four rates the radio's menu offers are honoured; anything
    // else (absent, hand-edited) falls to the default (Principle VII).
    const int v = readObj().value(QLatin1String(kFieldBaudRate)).toInt(0);
    if (v == 4800 || v == 9600 || v == 19200 || v == 38400)
        return v;
    return kDefaultBaud;
}

void Ft991Settings::setBaudRate(int baud)
{
    if (baud != 4800 && baud != 9600 && baud != 19200 && baud != 38400)
        return;
    QJsonObject o = readObj();
    o[QLatin1String(kFieldBaudRate)] = baud;
    writeObj(o);
}

QString Ft991Settings::audioInHint()
{
    const QString v =
        readObj().value(QLatin1String(kFieldAudioInHint)).toString();
    return v.isEmpty() ? QStringLiteral("usb audio codec") : v;
}

QString Ft991Settings::audioOutHint()
{
    const QString v =
        readObj().value(QLatin1String(kFieldAudioOutHint)).toString();
    return v.isEmpty() ? QStringLiteral("usb audio codec") : v;
}

}  // namespace AetherSDR
