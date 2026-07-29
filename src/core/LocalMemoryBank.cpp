#include "core/LocalMemoryBank.h"

#include "core/LocalMemoryStore.h"
#include "core/LogManager.h"
#include "core/backends/MemoryWireCodec.h"

#include <QDateTime>

namespace AetherSDR {

namespace {

// Writes are coalesced: a CSV import runs create+set per record back to back,
// and rewriting the whole file on each one would turn a 500-channel import into
// 1000 file writes. Every path that could lose the window (disconnect,
// teardown) calls flush().
constexpr int kSaveDebounceMs = 750;

// Internal sentinel for a memory command the local bank rejected. Not a
// SmartSDR protocol response code — it just has to be non-zero, which is what
// the client's response callback reads as a failure, and distinguishable in a
// log. Numbered alongside kProfileLoadSuppressedCommandCode (0x50000061).
constexpr int kLocalMemoryError = 0x50000062;

}  // namespace

LocalMemoryBank::LocalMemoryBank(QObject* parent)
    : QObject(parent)
{
    m_filePath = LocalMemoryStore::defaultFilePath();
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(kSaveDebounceMs);
    connect(&m_saveTimer, &QTimer::timeout, this, &LocalMemoryBank::flush);
}

LocalMemoryBank::~LocalMemoryBank()
{
    flush();
}

void LocalMemoryBank::setFilePath(const QString& path)
{
    if (path == m_filePath)
        return;
    // Anything pending belongs to the OLD file — write it there before moving,
    // or switching paths (only tests do today) would silently discard it.
    flush();
    m_filePath = path;
    m_loaded = false;
    m_entries.clear();
}

void LocalMemoryBank::load()
{
    if (m_loaded)
        return;

    const LocalMemoryStore::ParseResult parsed = LocalMemoryStore::load(m_filePath);
    m_entries = parsed.memories;
    m_loaded = true;
    m_dirty = false;

    if (!parsed.ok()) {
        m_lastError = parsed.errors.join(QStringLiteral("; "));
        qCWarning(lcProtocol).noquote()
            << "LocalMemoryBank: problems reading" << m_filePath << "—" << m_lastError;
        // A version-too-new file parses to nothing. Do NOT let a later edit
        // save over it: that would replace channels this build cannot read with
        // the empty bank it read instead.
        if (parsed.version > LocalMemoryStore::kFormatVersion)
            m_loaded = false;
        return;
    }

    m_lastError.clear();
    qCInfo(lcProtocol).noquote()
        << "LocalMemoryBank: loaded" << m_entries.size() << "memories from" << m_filePath;
}

int LocalMemoryBank::allocateSlot() const
{
    // Lowest free index, so removing a channel and adding another reuses the
    // hole instead of growing the numbering forever. Matches how the slot
    // numbers read in the browse panel.
    int index = 0;
    while (m_entries.contains(index))
        ++index;
    return index;
}

LocalMemoryBank::CommandResult LocalMemoryBank::handleCommand(const QString& command)
{
    CommandResult result;

    const QString trimmed = command.trimmed();
    if (!trimmed.startsWith(QLatin1String("memory ")))
        return result;

    const QString rest = trimmed.mid(7).trimmed();
    const QString verb = rest.section(QLatin1Char(' '), 0, 0);
    const QString tail = rest.section(QLatin1Char(' '), 1).trimmed();

    // The bank must be readable before it is written, or a create would
    // allocate slot 0 on top of an existing channel list.
    load();

    auto reject = [&result](const QString& reason) {
        result.handled = true;
        result.code = kLocalMemoryError;
        result.body = reason;
        return result;
    };

    // The index argument for every verb that takes one.
    auto slotArgument = [&tail](bool* ok) {
        return tail.section(QLatin1Char(' '), 0, 0).toInt(ok);
    };

    if (verb == QLatin1String("create")) {
        if (m_entries.size() >= kMaxSlots) {
            return reject(QStringLiteral("The memory bank is full (%1 channels).")
                              .arg(kMaxSlots));
        }

        const int index = allocateSlot();
        MemoryEntry entry;
        entry.index = index;
        m_entries.insert(index, entry);
        scheduleSave();

        result.handled = true;
        result.code = 0;
        // The client parses this as the new slot id, exactly as it parses the
        // radio's create response.
        result.body = QString::number(index);
        result.delta = MemoryWire::decodeStatus(index, {});
        return result;
    }

    if (verb == QLatin1String("set")) {
        bool ok = false;
        const int index = slotArgument(&ok);
        if (!ok)
            return reject(QStringLiteral("The memory id isn't a number."));
        if (!m_entries.contains(index))
            return reject(QString("There is no memory in slot %1.").arg(index));

        const QString kvTail = tail.section(QLatin1Char(' '), 1).trimmed();
        const QMap<QString, QString> kvs = MemoryWire::parseKvTail(kvTail);
        if (kvs.isEmpty())
            return reject(QStringLiteral("The memory update had no fields to set."));

        result.handled = true;
        result.code = 0;
        // RadioModel applies this and calls record() with the decoded entry —
        // that is what reaches the file, not the wire-encoded text here.
        result.delta = MemoryWire::decodeStatus(index, kvs);
        return result;
    }

    if (verb == QLatin1String("remove")) {
        bool ok = false;
        const int index = slotArgument(&ok);
        if (!ok)
            return reject(QStringLiteral("The memory id isn't a number."));
        // Removing something already gone is not an error. The dialog's
        // create→set→remove cleanup path can arrive here after the slot went,
        // and failing it would report a cleanup problem that does not exist.
        forget(index);

        result.handled = true;
        result.code = 0;
        MemoryDelta delta;
        delta.index = index;
        delta.removed = true;
        result.delta = delta;
        return result;
    }

    if (verb == QLatin1String("apply")) {
        bool ok = false;
        const int index = slotArgument(&ok);
        if (!ok)
            return reject(QStringLiteral("The memory id isn't a number."));
        if (!m_entries.contains(index))
            return reject(QString("There is no memory in slot %1.").arg(index));

        result.handled = true;
        result.code = 0;
        // No radio-side apply exists — the caller recalls it onto the active
        // slice itself.
        result.recallIndex = index;
        return result;
    }

    // Some other `memory …` verb. Leave it alone rather than answering for it.
    return result;
}

void LocalMemoryBank::record(int index, const MemoryEntry& entry)
{
    if (index < 0)
        return;

    MemoryEntry stored = entry;
    stored.index = index;

    // The dialog re-asserts the kv-set it just sent (handleMemoryStatus), so an
    // unconditional insert would mark the bank dirty twice per edit.
    const auto existing = m_entries.constFind(index);
    if (existing != m_entries.constEnd() && existing.value() == stored)
        return;

    m_entries.insert(index, stored);
    scheduleSave();
}

void LocalMemoryBank::forget(int index)
{
    if (m_entries.remove(index) > 0)
        scheduleSave();
}

void LocalMemoryBank::scheduleSave()
{
    m_dirty = true;
    m_saveTimer.start();
}

void LocalMemoryBank::flush()
{
    m_saveTimer.stop();
    if (!m_dirty)
        return;

    // Not loaded means load() refused to take ownership of the file (a version
    // this build cannot read). Writing would destroy it.
    if (!m_loaded) {
        qCWarning(lcProtocol).noquote()
            << "LocalMemoryBank: refusing to overwrite an unreadable bank at" << m_filePath;
        return;
    }

    QString error;
    const QString savedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    if (!LocalMemoryStore::save(m_filePath, m_entries, savedAt, &error)) {
        m_lastError = error;
        qCWarning(lcProtocol).noquote() << "LocalMemoryBank: save failed —" << error;
        emit saveFailed(error);
        // Stay dirty: the next edit (or flush) retries. A transient failure —
        // a config dir that is briefly unwritable — must not cost the operator
        // every channel they saved since.
        return;
    }

    m_dirty = false;
    m_lastError.clear();
    qCDebug(lcProtocol).noquote()
        << "LocalMemoryBank: saved" << m_entries.size() << "memories to" << m_filePath;
}

}  // namespace AetherSDR
