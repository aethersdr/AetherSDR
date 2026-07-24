#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariant>

// Copy Assist owns its configuration as one self-contained object stored under a
// single AppSettings root key ("CopyAssist"), per Constitution Principle V —
// never scattered as loose flat "Asr*" keys across the shared namespace. These
// accessors read/write fields within that nested object. Values keep the same
// string encoding the legacy flat keys used ("True"/"False", integer strings),
// and the nested field names are the legacy key names verbatim, so migration is
// a value-preserving move (not a rename) and callers keep their existing
// .toString()/.toInt()/=="True" logic. A one-time migration folds any
// pre-existing flat keys into the object and removes them.
namespace AetherSDR {
namespace CopyAssistSettings {

// The single AppSettings key holding the whole config object.
inline QString rootKey()
{
    return QStringLiteral("CopyAssist");
}

// The legacy flat AppSettings keys folded into the object. Kept verbatim as the
// nested field names so a value never has to be re-encoded or renamed.
inline const QStringList& legacyFlatKeys()
{
    static const QStringList kKeys = {
        QStringLiteral("AsrLanguage"),        QStringLiteral("AsrGpuDevice"),
        QStringLiteral("AsrRemoteUrl"),       QStringLiteral("AsrRemoteApiKey"),
        QStringLiteral("AsrRemoteModel"),     QStringLiteral("AsrRemoteEnabled"),
        QStringLiteral("AsrCustomModelPath"), QStringLiteral("AsrSherpaModelDir"),
        QStringLiteral("AsrLogFilePath"),     QStringLiteral("AsrLogToFile"),
        QStringLiteral("AsrVadModelPath"),    QStringLiteral("AsrVadEnabled"),
        QStringLiteral("AsrSpeakerModelPath"),QStringLiteral("AsrSpeakerThreshold"),
        QStringLiteral("AsrSpeakerEnabled"),  QStringLiteral("AsrDecodeBufferMs"),
        QStringLiteral("AsrSensitivity"),     QStringLiteral("AsrSilenceMs"),
        QStringLiteral("AsrFontPx"),          QStringLiteral("AsrNewlineOnSilence"),
        QStringLiteral("AsrPanelHeight"),
    };
    return kKeys;
}

// Pure: fold present legacy flat entries into `nested`, without overwriting a
// field already there (a value already migrated wins). `present` maps each
// legacy flat key that EXISTS to its stored string value. Returns the merged
// object. Header-inline and singleton-free so it is unit testable.
inline QJsonObject foldLegacyKeys(const QMap<QString, QString>& present, QJsonObject nested)
{
    for (const QString& key : legacyFlatKeys()) {
        const auto it = present.find(key);
        if (it != present.end() && !nested.contains(key)) {
            nested.insert(key, it.value());
        }
    }
    return nested;
}

// Field accessors on the nested object (field == the legacy key name, e.g.
// "AsrLanguage"). Reads/writes go through AppSettings; setValue() persists.
QVariant value(const QString& field, const QVariant& defaultValue = {});
void setValue(const QString& field, const QVariant& val);

} // namespace CopyAssistSettings
} // namespace AetherSDR
