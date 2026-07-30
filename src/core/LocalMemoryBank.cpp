#include "core/LocalMemoryBank.h"

#include "core/LocalMemoryStore.h"
#include "core/LogManager.h"
#include "core/backends/MemoryWireCodec.h"

#include <QDateTime>
#include <QFileInfo>

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
    m_dirty = false;

    // TWO facts, deliberately separate, because one flag could not carry both:
    //
    //   m_loaded   — the read has been ATTEMPTED. Latches true no matter how the
    //                read went, so nothing re-reads.
    //   m_writable — the file was UNDERSTOOD, so replacing it is safe.
    //
    // Using m_loaded for both was wrong in both directions. Leaving it true on a
    // failed read made the bank writable, so the next flush() replaced a damaged
    // file with the empty bank it had just parsed out of it. And clearing it to
    // withhold write permission also cleared "already read", so handleCommand()'s
    // load() re-ran on EVERY command and wiped m_entries between `memory create`
    // and `memory set` — every Add failing with "There is no memory in slot N",
    // and the reset m_dirty suppressing the warning that would have explained it.
    m_loaded = true;
    m_writable = parsed.overwritable();
    // Baseline for the concurrent-writer check in flush().
    rememberFileState();

    if (!parsed.ok()) {
        m_lastError = parsed.errors.join(QStringLiteral("; "));
        qCWarning(lcProtocol).noquote()
            << "LocalMemoryBank: problems reading" << m_filePath << "—" << m_lastError
            << (m_writable ? "(bank is still writable)"
                           : "(bank is READ-ONLY; edits will be refused)");
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

    // Refuse every write up front when the file could not be read, and say why.
    // Without this the command runs against an in-memory bank that flush() will
    // then decline to persist, so the operator sees an Add "succeed" and vanish
    // at restart. Naming the file is the point — the fix is theirs to make.
    if (!m_writable) {
        return reject(QString("The memory file at %1 could not be read, so it "
                              "will not be overwritten: %2")
                          .arg(m_filePath, m_lastError));
    }

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

    // Not writable means load() could not understand the file — a version this
    // build cannot read, a foreign format id, or JSON it could not parse.
    // Writing would destroy it.
    if (!m_writable) {
        qCWarning(lcProtocol).noquote()
            << "LocalMemoryBank: refusing to overwrite an unreadable bank at" << m_filePath;
        return;
    }

    // Somebody else wrote the file since we read it.
    //
    // The bank is replaced WHOLESALE and read once per process, so a second
    // instance sharing the same config dir (two AetherSDR windows, a test
    // instance on the same $HOME) would have its channels silently discarded by
    // whichever process saved last. Refusing is deliberately not a merge: which
    // side wins per slot is a design decision, not something to infer here. This
    // turns silent loss into a visible, recoverable error — the entries stay in
    // memory and dirty, and the operator is told rather than finding out later.
    if (foreignWriteDetected()) {
        m_lastError = QString("The memory file at %1 was changed by another "
                              "program or window since it was read. Nothing was "
                              "saved, so neither copy is lost — reopen the memory "
                              "panel to pick up the other changes.")
                          .arg(m_filePath);
        qCWarning(lcProtocol).noquote() << "LocalMemoryBank:" << m_lastError;
        emit saveFailed(m_lastError);
        return;   // stays dirty
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
    // Our own write moved the mtime; re-baseline or the NEXT flush would mistake
    // it for someone else's.
    rememberFileState();
    qCDebug(lcProtocol).noquote()
        << "LocalMemoryBank: saved" << m_entries.size() << "memories to" << m_filePath;
}

void LocalMemoryBank::rememberFileState()
{
    const QFileInfo info(m_filePath);
    m_seenExists = info.exists();
    m_seenMtime = m_seenExists ? info.lastModified() : QDateTime();
    m_seenSize = m_seenExists ? info.size() : -1;
}

bool LocalMemoryBank::foreignWriteDetected() const
{
    const QFileInfo info(m_filePath);
    // Appearing counts: we read "no file, empty bank", so a file that exists now
    // holds channels somebody else created, and our empty-ish bank would erase
    // them. Disappearing does not — a deleted bank is nothing to preserve, and
    // refusing there would strand the operator's edits with nowhere to go.
    if (!info.exists())
        return false;
    if (!m_seenExists)
        return true;
    // Size as well as mtime: a coarse filesystem timestamp can collide on two
    // writes inside the same tick, and a channel edit almost always changes the
    // length. Neither is a substitute for a lock, and this does not claim to be
    // one — it catches the realistic case (a human editing in two windows),
    // not a deliberate race.
    return info.lastModified() != m_seenMtime || info.size() != m_seenSize;
}

}  // namespace AetherSDR
