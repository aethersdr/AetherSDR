#include "UlanziDialMappings.h"

#include "core/AppSettings.h"
#include "core/LogManager.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace AetherSDR {

namespace {

// Parse the stored document.  A missing or empty value is a legitimately empty
// document; anything else that fails to parse is corruption worth a warning,
// and we start from empty rather than propagating garbage.
QJsonObject readDocument(const char* context)
{
    const QByteArray raw = AppSettings::instance()
                               .value(UlanziDialMappings::rootSettingsKey())
                               .toString().toUtf8();
    if (raw.isEmpty())
        return {};
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (err.error == QJsonParseError::NoError && doc.isObject())
        return doc.object();
    qCWarning(lcDevices) << "Ulanzi Dial: unparseable mappings document under"
                         << UlanziDialMappings::rootSettingsKey() << "—"
                         << err.errorString() << "— starting from empty (" << context << ")";
    return {};
}

bool writeDocument(const QJsonObject& obj, const QString& what)
{
    auto& s = AppSettings::instance();
    const QString json =
        QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    s.setValue(UlanziDialMappings::rootSettingsKey(), json);
    s.save();

    // Evidence over assertion: a failed save() deliberately keeps the change
    // in memory for retry, so a cache re-read would match even if nothing was
    // committed.  Only a file read proves persistence.
    QString onDisk;
    if (!s.readAppRowFromDisk(UlanziDialMappings::rootSettingsKey(), onDisk) || onDisk != json) {
        qCWarning(lcDevices) << "Ulanzi Dial: failed to persist" << what << "under"
                             << UlanziDialMappings::rootSettingsKey()
                             << "— the binding will not survive a restart";
        return false;
    }
    return true;
}

}  // namespace

QString UlanziDialMappings::rootSettingsKey()
{
    return QStringLiteral("UlanziDialMappings");
}

QString UlanziDialMappings::legacyUnderscoreKey(const QString& pillId)
{
    return QStringLiteral("UlanziDial_action_%1").arg(pillId);
}

QString UlanziDialMappings::legacySlashKey(const QString& pillId)
{
    return QStringLiteral("UlanziDial/action/%1").arg(pillId);
}

QString UlanziDialMappings::actionForPill(const QString& pillId)
{
    const QJsonObject obj = readDocument("read");
    if (!obj.contains(pillId))
        return {};
    return obj.value(pillId).toString();
}

bool UlanziDialMappings::setActionForPill(const QString& pillId, const QString& actionId)
{
    QJsonObject obj = readDocument("write");
    obj.insert(pillId, actionId);
    return writeDocument(obj, QStringLiteral("mapping for pill '%1'").arg(pillId));
}

int UlanziDialMappings::migrateLegacyKeys(const QStringList& pillIds)
{
    auto& s = AppSettings::instance();
    QJsonObject obj = readDocument("migrate");
    int adopted = 0;
    bool touched = false;

    for (const QString& pillId : pillIds) {
        const QString underscore = legacyUnderscoreKey(pillId);
        const QString slash = legacySlashKey(pillId);
        const bool hasUnderscore = s.contains(underscore);
        const bool hasSlash = s.contains(slash);
        if (!hasUnderscore && !hasSlash)
            continue;

        // The document wins where it already has an opinion — including an
        // opinion the operator formed by clearing a binding.  The legacy key
        // is still removed, so it cannot reassert itself later.
        if (!obj.contains(pillId)) {
            const QString legacy = hasUnderscore ? s.value(underscore).toString()
                                                 : s.value(slash).toString();
            if (!legacy.isEmpty()) {
                obj.insert(pillId, legacy);
                ++adopted;
            }
        }
        if (hasUnderscore) s.remove(underscore);
        if (hasSlash) s.remove(slash);
        touched = true;
    }

    if (!touched)
        return 0;

    // One write for the whole sweep, not one per pill.
    if (writeDocument(obj, QStringLiteral("migrated mappings")) && adopted > 0) {
        qCInfo(lcDevices) << "Ulanzi Dial: migrated" << adopted
                          << "legacy pill binding(s) into" << rootSettingsKey();
    }
    return adopted;
}

}  // namespace AetherSDR
