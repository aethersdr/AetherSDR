#include "gui/CopyAssistSettings.h"

#include "core/AppSettings.h"

#include <QJsonDocument>

namespace AetherSDR {
namespace CopyAssistSettings {

namespace {

QJsonObject readObject()
{
    return QJsonDocument::fromJson(
               AppSettings::instance().value(rootKey()).toString().toUtf8())
        .object();
}

// One-time: fold any legacy flat Asr* keys into the nested object and delete
// them, persisting the whole change atomically. Idempotent and cheap after the
// first run (nothing left to migrate). Guarded by a process-lifetime flag so it
// runs at most once regardless of which accessor is hit first (the panel reads
// AsrPanelHeight independently of the controller).
void ensureMigrated()
{
    static bool done = false;
    if (done) {
        return;
    }

    auto& s = AppSettings::instance();
    QMap<QString, QString> present;
    for (const QString& key : legacyFlatKeys()) {
        if (s.contains(key)) {
            present.insert(key, s.value(key).toString());
        }
    }
    if (present.isEmpty()) {
        // Nothing to fold. Latch `done` only once we can confirm this is a real
        // loaded state — the nested root key already exists (migration ran, or a
        // fresh install has since written a field). If BOTH the flat keys and the
        // root object are absent we may simply be pre-load, so leave `done` unset
        // and retry on the next access rather than permanently skipping migration
        // and orphaning the user's settings.
        if (s.contains(rootKey())) {
            done = true;
        }
        return;
    }
    done = true;
    const QJsonObject merged = foldLegacyKeys(present, readObject());
    s.setValue(rootKey(), QString::fromUtf8(QJsonDocument(merged).toJson(QJsonDocument::Compact)));
    for (auto it = present.constBegin(); it != present.constEnd(); ++it) {
        s.remove(it.key());
    }
    s.save();
}

} // namespace

QVariant value(const QString& field, const QVariant& defaultValue)
{
    ensureMigrated();
    const QJsonObject obj = readObject();
    const QJsonValue v = obj.value(field);
    return v.isUndefined() ? defaultValue : QVariant(v.toString());
}

void setValue(const QString& field, const QVariant& val)
{
    ensureMigrated();
    QJsonObject obj = readObject();
    obj.insert(field, val.toString());
    auto& s = AppSettings::instance();
    s.setValue(rootKey(), QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    s.save();
}

} // namespace CopyAssistSettings
} // namespace AetherSDR
