#include "core/LocalMemoryStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace AetherSDR {

namespace {

// Every MemoryEntry field is written — this is the bank's own canonical form,
// not the portable net-schedule preset, so a load has to reproduce the slot
// exactly as the operator saved it. `index` rides in the object rather than
// being implied by array position; see the header.
QJsonObject entryToJson(const MemoryEntry& m)
{
    QJsonObject o;
    o["index"] = m.index;
    o["group"] = m.group;
    o["owner"] = m.owner;
    o["channel"] = m.channel;
    o["importSource"] = m.importSource;
    o["importKey"] = m.importKey;
    o["freq"] = m.freq;
    o["name"] = m.name;
    o["mode"] = m.mode;
    o["nativeFilter"] = m.nativeFilter;
    o["dataMode"] = m.dataMode;
    o["step"] = m.step;
    o["offsetDir"] = m.offsetDir;
    o["repeaterOffset"] = m.repeaterOffset;
    o["toneMode"] = m.toneMode;
    o["toneValue"] = m.toneValue;
    o["rxToneValue"] = m.rxToneValue;
    o["dtcsCode"] = m.dtcsCode;
    o["dtcsTxReverse"] = m.dtcsTxReverse;
    o["dtcsRxReverse"] = m.dtcsRxReverse;
    o["recallable"] = m.recallable;
    o["squelch"] = m.squelch;
    o["squelchLevel"] = m.squelchLevel;
    o["rxFilterLow"] = m.rxFilterLow;
    o["rxFilterHigh"] = m.rxFilterHigh;
    o["rttyMark"] = m.rttyMark;
    o["rttyShift"] = m.rttyShift;
    o["diglOffset"] = m.diglOffset;
    o["diguOffset"] = m.diguOffset;
    return o;
}

MemoryEntry entryFromJson(const QJsonObject& o)
{
    MemoryEntry m;
    m.index = o.value("index").toInt(m.index);
    m.group = o.value("group").toString(m.group);
    m.owner = o.value("owner").toString(m.owner);
    m.channel = o.value("channel").toString(m.channel);
    m.importSource = o.value("importSource").toString(m.importSource);
    m.importKey = o.value("importKey").toString(m.importKey);
    m.freq = o.value("freq").toDouble(m.freq);
    m.name = o.value("name").toString(m.name);
    m.mode = o.value("mode").toString(m.mode);
    m.nativeFilter = o.value("nativeFilter").toInt(m.nativeFilter);
    m.dataMode = o.value("dataMode").toInt(m.dataMode);
    m.step = o.value("step").toInt(m.step);
    m.offsetDir = o.value("offsetDir").toString(m.offsetDir);
    m.repeaterOffset = o.value("repeaterOffset").toDouble(m.repeaterOffset);
    m.toneMode = o.value("toneMode").toString(m.toneMode);
    m.toneValue = o.value("toneValue").toDouble(m.toneValue);
    m.rxToneValue = o.value("rxToneValue").toDouble(m.rxToneValue);
    m.dtcsCode = o.value("dtcsCode").toInt(m.dtcsCode);
    m.dtcsTxReverse = o.value("dtcsTxReverse").toBool(m.dtcsTxReverse);
    m.dtcsRxReverse = o.value("dtcsRxReverse").toBool(m.dtcsRxReverse);
    m.recallable = o.value("recallable").toBool(m.recallable);
    m.squelch = o.value("squelch").toBool(m.squelch);
    m.squelchLevel = o.value("squelchLevel").toInt(m.squelchLevel);
    m.rxFilterLow = o.value("rxFilterLow").toInt(m.rxFilterLow);
    m.rxFilterHigh = o.value("rxFilterHigh").toInt(m.rxFilterHigh);
    m.rttyMark = o.value("rttyMark").toInt(m.rttyMark);
    m.rttyShift = o.value("rttyShift").toInt(m.rttyShift);
    m.diglOffset = o.value("diglOffset").toInt(m.diglOffset);
    m.diguOffset = o.value("diguOffset").toInt(m.diguOffset);
    return m;
}

}  // namespace

int LocalMemoryStore::formatVersionFor(const QMap<int, MemoryEntry>& memories)
{
    for (const MemoryEntry& memory : memories) {
        if (!memory.channel.isEmpty() || !memory.importSource.isEmpty()
            || !memory.importKey.isEmpty() || memory.nativeFilter != 0
            || memory.dataMode != 0 || memory.rxToneValue != 0.0
            || memory.dtcsCode != 23 || memory.dtcsTxReverse
            || memory.dtcsRxReverse || !memory.recallable) {
            return kFormatVersion;
        }
    }
    return 1;
}

QByteArray LocalMemoryStore::serialize(const QMap<int, MemoryEntry>& memories,
                                       const QString& savedAtIso)
{
    QJsonObject root;
    root["format"] = kFormatId;
    root["version"] = formatVersionFor(memories);
    if (!savedAtIso.isEmpty())
        root["savedAt"] = savedAtIso;
    root["savedBy"] = "AetherSDR";

    // QMap iterates in key order, so the array comes out sorted by slot index.
    QJsonArray arr;
    for (auto it = memories.constBegin(); it != memories.constEnd(); ++it) {
        MemoryEntry entry = it.value();
        entry.index = it.key();   // the map key is authoritative
        arr.append(entryToJson(entry));
    }
    root["memories"] = arr;

    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

LocalMemoryStore::ParseResult LocalMemoryStore::parse(const QByteArray& bytes)
{
    ParseResult result;

    // An empty file is an empty bank, not a corrupt one. Treating it as an
    // error would make a zero-byte file left by a full disk look like data
    // loss the operator has to act on.
    if (bytes.trimmed().isEmpty())
        return result;

    // Every branch below that cannot make sense of the file marks it UNREADABLE,
    // not merely errored. A caller may not overwrite what it could not read: the
    // bytes are channels somebody saved, and replacing them with the empty bank
    // this parse produced is data loss, not a lost field.
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &perr);
    if (doc.isNull()) {
        result.errors << QString("Invalid JSON: %1").arg(perr.errorString());
        result.unreadable = true;
        return result;
    }
    if (!doc.isObject()) {
        result.errors << QStringLiteral("Top-level JSON is not an object");
        result.unreadable = true;
        return result;
    }

    const QJsonObject root = doc.object();
    const QString format = root.value("format").toString();
    // A MISSING format id is tolerated — an early hand-written file, and the
    // memories still parse. A PRESENT but foreign one is another application's
    // file that happens to live at our path, so stop: reading its "memories"
    // array and then saving our envelope over it would clobber it.
    if (!format.isEmpty() && format != QLatin1String(kFormatId)) {
        result.errors << QString("Unexpected format \"%1\"").arg(format);
        result.unreadable = true;
        return result;
    }

    result.version = root.value("version").toInt(0);
    if (result.version > kFormatVersion) {
        // Fail the whole read. Parsing what this build understands and then
        // saving over the file would silently delete every field a newer
        // version added.
        result.errors << QString("File version %1 is newer than supported version %2")
                             .arg(result.version)
                             .arg(kFormatVersion);
        result.unreadable = true;
        return result;
    }

    const QJsonArray arr = root.value("memories").toArray();
    for (const QJsonValue& v : arr) {
        if (!v.isObject())
            continue;
        const MemoryEntry entry = entryFromJson(v.toObject());
        // A slot with no usable index has no handle the UX could address it by,
        // and silently renumbering it would move somebody else's channel.
        if (entry.index < 0) {
            result.errors << QStringLiteral("Skipped a memory with no valid slot index");
            continue;
        }
        if (result.memories.contains(entry.index)) {
            result.errors << QString("Skipped a duplicate memory for slot %1")
                                 .arg(entry.index);
            continue;
        }
        result.memories.insert(entry.index, entry);
    }

    return result;
}

QString LocalMemoryStore::defaultFilePath()
{
    // GenericConfigLocation, matching AppSettings: a plain base dir on every
    // platform (~/.config on Linux/macOS, %LOCALAPPDATA% on Windows) with no
    // org/app suffix, so appending "/AetherSDR" cannot produce the triple-nested
    // path ConfigLocation gives on Windows and macOS.
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty())
        return {};
    return base + QStringLiteral("/AetherSDR/memories.json");
}

LocalMemoryStore::ParseResult LocalMemoryStore::load(const QString& path)
{
    ParseResult result;
    if (path.isEmpty()) {
        result.errors << QStringLiteral("No memory bank path is available");
        return result;
    }

    QFile file(path);
    if (!file.exists())
        return result;   // first run — an empty bank, not a failure

    if (!file.open(QIODevice::ReadOnly)) {
        result.errors << QString("Couldn't open %1: %2").arg(path, file.errorString());
        return result;
    }
    return parse(file.readAll());
}

bool LocalMemoryStore::save(const QString& path,
                            const QMap<int, MemoryEntry>& memories,
                            const QString& savedAtIso,
                            QString* error)
{
    auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };

    if (path.isEmpty())
        return fail(QStringLiteral("No memory bank path is available"));

    const QString dir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dir))
        return fail(QString("Couldn't create %1").arg(dir));

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QString("Couldn't write %1: %2").arg(path, file.errorString()));

    const QByteArray bytes = serialize(memories, savedAtIso);
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return fail(QString("Couldn't write %1: %2").arg(path, file.errorString()));
    }
    if (!file.commit())
        return fail(QString("Couldn't commit %1: %2").arg(path, file.errorString()));

    if (error)
        error->clear();
    return true;
}

}  // namespace AetherSDR
