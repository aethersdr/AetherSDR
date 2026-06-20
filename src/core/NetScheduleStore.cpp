#include "core/NetScheduleStore.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUuid>

namespace AetherSDR {

namespace {

// --- MemoryEntry preset <-> JSON ----------------------------------------
//
// Only the fields a net actually needs for recall are serialized; the rest take
// MemoryEntry's defaults. group/owner/index are radio-slot concepts and
// deliberately omitted from the portable net format.

QJsonObject presetToJson(const MemoryEntry& m)
{
    QJsonObject o;
    o["freq"] = m.freq;
    o["mode"] = m.mode;
    o["step"] = m.step;
    o["rxFilterLow"] = m.rxFilterLow;
    o["rxFilterHigh"] = m.rxFilterHigh;
    if (!m.offsetDir.isEmpty())
        o["offsetDir"] = m.offsetDir;
    if (m.repeaterOffset != 0.0)
        o["repeaterOffset"] = m.repeaterOffset;
    if (!m.toneMode.isEmpty())
        o["toneMode"] = m.toneMode;
    if (m.toneValue != 0.0)
        o["toneValue"] = m.toneValue;
    if (m.squelch) {
        o["squelch"] = true;
        o["squelchLevel"] = m.squelchLevel;
    }
    return o;
}

MemoryEntry presetFromJson(const QJsonObject& o)
{
    MemoryEntry m;
    m.freq = o.value("freq").toDouble(m.freq);
    m.mode = o.value("mode").toString(m.mode);
    m.step = o.value("step").toInt(m.step);
    m.rxFilterLow = o.value("rxFilterLow").toInt(m.rxFilterLow);
    m.rxFilterHigh = o.value("rxFilterHigh").toInt(m.rxFilterHigh);
    m.offsetDir = o.value("offsetDir").toString(m.offsetDir);
    m.repeaterOffset = o.value("repeaterOffset").toDouble(m.repeaterOffset);
    m.toneMode = o.value("toneMode").toString(m.toneMode);
    m.toneValue = o.value("toneValue").toDouble(m.toneValue);
    m.squelch = o.value("squelch").toBool(m.squelch);
    m.squelchLevel = o.value("squelchLevel").toInt(m.squelchLevel);
    return m;
}

QJsonObject entryToJson(const NetEntry& e)
{
    QJsonObject o;
    o["id"] = e.id;
    o["name"] = e.name;
    o["enabled"] = e.enabled;
    o["rrule"] = e.rrule;
    if (!e.startDate.isEmpty())
        o["startDate"] = e.startDate;
    o["timeOfDay"] = e.timeOfDay;
    o["timezone"] = e.timezone;
    o["reminderLeadMinutes"] = e.reminderLeadMinutes;
    o["durationMinutes"] = e.durationMinutes;
    if (!e.notes.isEmpty())
        o["notes"] = e.notes;
    o["preset"] = presetToJson(e.preset);
    return o;
}

NetEntry entryFromJson(const QJsonObject& o)
{
    NetEntry e;
    e.id = o.value("id").toString();
    e.name = o.value("name").toString();
    e.enabled = o.value("enabled").toBool(true);
    e.rrule = o.value("rrule").toString();
    e.startDate = o.value("startDate").toString();
    e.timeOfDay = o.value("timeOfDay").toString(e.timeOfDay);
    e.timezone = o.value("timezone").toString(e.timezone);
    e.reminderLeadMinutes = o.value("reminderLeadMinutes").toInt(e.reminderLeadMinutes);
    e.durationMinutes = o.value("durationMinutes").toInt(e.durationMinutes);
    e.notes = o.value("notes").toString();
    e.preset = presetFromJson(o.value("preset").toObject());
    return e;
}

} // namespace

QString NetScheduleStore::newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QByteArray NetScheduleStore::serialize(const QList<NetEntry>& nets, const QString& exportedAtIso)
{
    QJsonObject root;
    root["format"] = kFormatId;
    root["version"] = kFormatVersion;
    if (!exportedAtIso.isEmpty())
        root["exportedAt"] = exportedAtIso;
    root["exportedBy"] = "AetherSDR";

    QJsonArray arr;
    for (const NetEntry& e : nets)
        arr.append(entryToJson(e));
    root["nets"] = arr;

    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

NetScheduleStore::ParseResult NetScheduleStore::parse(const QByteArray& bytes)
{
    ParseResult result;

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &perr);
    if (doc.isNull()) {
        result.errors << QString("Invalid JSON: %1").arg(perr.errorString());
        return result;
    }
    if (!doc.isObject()) {
        result.errors << "Top-level JSON is not an object";
        return result;
    }

    const QJsonObject root = doc.object();
    const QString format = root.value("format").toString();
    if (!format.isEmpty() && format != kFormatId)
        result.errors << QString("Unexpected format \"%1\"").arg(format);

    result.version = root.value("version").toInt(0);
    if (result.version > kFormatVersion) {
        result.errors << QString("File version %1 is newer than supported version %2")
                             .arg(result.version)
                             .arg(kFormatVersion);
    }

    const QJsonArray arr = root.value("nets").toArray();
    for (const QJsonValue& v : arr) {
        if (!v.isObject())
            continue;
        NetEntry e = entryFromJson(v.toObject());
        if (e.id.isEmpty())
            e.id = newId();
        result.nets.append(e);
    }

    return result;
}

QList<NetEntry> NetScheduleStore::merge(const QList<NetEntry>& existing,
                                        const QList<NetEntry>& incoming,
                                        MergePolicy policy)
{
    QList<NetEntry> merged = existing;
    QHash<QString, int> indexById;
    for (int i = 0; i < merged.size(); ++i) {
        if (!merged.at(i).id.isEmpty())
            indexById.insert(merged.at(i).id, i);
    }

    for (const NetEntry& in : incoming) {
        NetEntry entry = in;
        if (entry.id.isEmpty())
            entry.id = newId();

        const auto it = indexById.constFind(entry.id);
        if (it == indexById.constEnd()) {
            indexById.insert(entry.id, merged.size());
            merged.append(entry);
            continue;
        }

        switch (policy) {
        case MergePolicy::Skip:
            break;
        case MergePolicy::Overwrite:
            merged[it.value()] = entry;
            break;
        case MergePolicy::Duplicate:
            entry.id = newId();
            indexById.insert(entry.id, merged.size());
            merged.append(entry);
            break;
        }
    }

    return merged;
}

} // namespace AetherSDR
