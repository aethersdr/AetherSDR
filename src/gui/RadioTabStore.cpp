#include "gui/RadioTabStore.h"

#include "core/AppSettings.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace AetherSDR {

namespace {
constexpr const char* kSettingsKey = "RadioTabs";

SavedRadio fromJson(const QJsonObject& o)
{
    SavedRadio r;
    r.id         = o.value(QStringLiteral("id")).toString();
    r.family     = o.value(QStringLiteral("family")).toString(QStringLiteral("flex"));
    r.name       = o.value(QStringLiteral("name")).toString();
    r.customName = o.value(QStringLiteral("customName")).toString();
    r.address    = o.value(QStringLiteral("address")).toString();
    r.port       = static_cast<quint16>(o.value(QStringLiteral("port")).toInt());
    r.serial     = o.value(QStringLiteral("serial")).toString();
    return r;
}

QJsonObject toJson(const SavedRadio& r)
{
    return QJsonObject{
        {QStringLiteral("id"),         r.id},
        {QStringLiteral("family"),     r.family},
        {QStringLiteral("name"),       r.name},
        {QStringLiteral("customName"), r.customName},
        {QStringLiteral("address"),    r.address},
        {QStringLiteral("port"),       int(r.port)},
        {QStringLiteral("serial"),     r.serial},
    };
}
} // namespace

QString SavedRadio::makeId(const QString& family, const QString& serial,
                           const QString& address)
{
    // The HL2's discovery "serial" is derived from its MAC and has been seen to
    // change across gateware revisions, so keying a tab on it would orphan the
    // tab after a firmware update. Its address is what the operator actually
    // types and what the backend reconnects with.
    if (family == QLatin1String("hl2")) {
        return QStringLiteral("hl2:") + address;
    }
    if (!serial.isEmpty()) {
        return serial;
    }
    // No serial (an Icom reached by address before it has answered, say) —
    // fall back to the same family:address shape rather than an empty key,
    // which would collide with every other serial-less radio.
    return family + QLatin1Char(':') + address;
}

QList<SavedRadio> RadioTabStore::load()
{
    QList<SavedRadio> out;
    const QString raw = AppSettings::instance().value(kSettingsKey, "").toString();
    if (raw.isEmpty()) {
        return out;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        // A corrupt blob is dropped rather than throwing the strip away on
        // every launch; the next successful connect rebuilds it.
        return out;
    }
    const QJsonArray arr = doc.array();
    out.reserve(arr.size());
    for (const QJsonValue& v : arr) {
        if (!v.isObject()) continue;
        SavedRadio r = fromJson(v.toObject());
        if (!r.id.isEmpty()) out.append(r);
    }
    return out;
}

void RadioTabStore::save(const QList<SavedRadio>& radios)
{
    QJsonArray arr;
    for (const SavedRadio& r : radios) {
        if (r.id.isEmpty()) continue;
        arr.append(toJson(r));
    }
    AppSettings::instance().setValue(
        kSettingsKey,
        QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    AppSettings::instance().save();
}

void RadioTabStore::upsert(const SavedRadio& radio)
{
    if (radio.id.isEmpty()) return;
    QList<SavedRadio> all = load();
    for (SavedRadio& r : all) {
        if (r.id != radio.id) continue;
        const QString keptName = r.customName;   // a rename outlives a reconnect
        r = radio;
        if (r.customName.isEmpty()) r.customName = keptName;
        save(all);
        return;
    }
    all.append(radio);
    save(all);
}

void RadioTabStore::remove(const QString& id)
{
    QList<SavedRadio> all = load();
    const auto before = all.size();
    all.removeIf([&id](const SavedRadio& r) { return r.id == id; });
    if (all.size() != before) save(all);
}

void RadioTabStore::rename(const QString& id, const QString& customName)
{
    QList<SavedRadio> all = load();
    for (SavedRadio& r : all) {
        if (r.id != id) continue;
        r.customName = customName.trimmed();
        save(all);
        return;
    }
}

bool RadioTabStore::contains(const QString& id)
{
    const QList<SavedRadio> all = load();
    for (const SavedRadio& r : all) {
        if (r.id == id) return true;
    }
    return false;
}

} // namespace AetherSDR
