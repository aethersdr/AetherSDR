#include "core/VoiceKeyerSettings.h"

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QStandardPaths>

namespace AetherSDR {

namespace {
const QString kVoiceKeyerKey = QStringLiteral("VoiceKeyer");
const QString kSourceField   = QStringLiteral("source");
const QString kSlotsField    = QStringLiteral("slots");
} // namespace

QJsonObject VoiceKeyerSettings::readObj()
{
    const QString json =
        AppSettings::instance().value(kVoiceKeyerKey, QString{}).toString();
    if (json.isEmpty())
        return {};
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

void VoiceKeyerSettings::write(const QJsonObject& o)
{
    auto& s = AppSettings::instance();
    s.setValue(kVoiceKeyerKey,
               QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    s.save();
}

VoiceKeyerSourceSetting VoiceKeyerSettings::source()
{
    return parseVoiceKeyerSourceSetting(readObj().value(kSourceField).toString());
}

void VoiceKeyerSettings::setSource(VoiceKeyerSourceSetting setting)
{
    QJsonObject o = readObj();
    if (setting == VoiceKeyerSourceSetting::Auto)
        o.remove(kSourceField);
    else
        o[kSourceField] = voiceKeyerSourceSettingName(setting);
    write(o);
}

QString VoiceKeyerSettings::slotName(int id)
{
    return readObj().value(kSlotsField).toObject()
        .value(QString::number(id)).toObject()
        .value(QStringLiteral("name")).toString();
}

void VoiceKeyerSettings::setSlotName(int id, const QString& name)
{
    QJsonObject o = readObj();
    QJsonObject slotMap = o.value(kSlotsField).toObject();
    const QString key = QString::number(id);
    QJsonObject slot = slotMap.value(key).toObject();
    if (name.isEmpty())
        slot.remove(QStringLiteral("name"));
    else
        slot[QStringLiteral("name")] = name;
    if (slot.isEmpty())
        slotMap.remove(key);
    else
        slotMap[key] = slot;
    o[kSlotsField] = slotMap;
    write(o);
}

QString VoiceKeyerSettings::recordingsDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
           + QStringLiteral("/AetherSDR/VoiceKeyer");
}

} // namespace AetherSDR
