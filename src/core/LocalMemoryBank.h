#pragma once

#include "core/backends/MemoryDelta.h"
#include "models/MemoryEntry.h"

#include <optional>

#include <QDateTime>
#include <QMap>
#include <QObject>
#include <QString>
#include <QTimer>

namespace AetherSDR {

// The client-side memory bank: what a radio's memory slots would be, for a
// radio that has none. Standing in for the radio is deliberate — it is what
// lets every existing memory path keep working untouched.
//
// The whole memory UX (the dialog, the browse panel, CSV import/export, the
// panadapter memory-spot feed, the automation `memory activate` verb) is built
// on two things: RadioModel's memory cache, and the four commands the client
// issues to change it — `memory create`, `memory set <idx> <kv…>`,
// `memory remove <idx>`, `memory apply <idx>`. On a Flex those go to the radio
// and come back as memory status. Here they are answered locally, in the same
// shape, with the same kv-set decode (MemoryWire::decodeStatus), so nothing
// upstream needs to know which kind of radio it is talking to.
//
// Ownership split with RadioModel is one-way and worth keeping straight:
//   * The bank answers commands and hands back a MemoryDelta.
//   * RadioModel applies that delta — it owns the 0x7f→' ' decode and the
//     control-byte sanitisation, and it emits the memoryChanged/memoryRemoved
//     signals the UI listens to.
//   * RadioModel then calls record() with its post-decode MemoryEntry, which is
//     what gets persisted.
// So the JSON on disk is byte-for-byte what the UI and a CSV export see, rather
// than a second, wire-encoded interpretation of it that could drift.
class LocalMemoryBank : public QObject {
    Q_OBJECT

public:
    // The answer to one intercepted `memory …` command.
    struct CommandResult {
        // False means "not a command this bank owns" — the caller lets it take
        // its normal path rather than swallowing it.
        bool handled{false};

        // Response the client's callback receives. Non-zero code is a failure
        // and `body` is the reason, matching how a radio rejection reads.
        int code{0};
        QString body;

        // Apply through RadioModel so the cache and signals stay on one path.
        std::optional<MemoryDelta> delta;

        // >= 0 for `memory apply` — the slot the caller should recall onto the
        // active slice. There is no radio to do it for us.
        int recallIndex{-1};
    };

    explicit LocalMemoryBank(QObject* parent = nullptr);
    ~LocalMemoryBank() override;

    // The LEGACY import source (RFC #4603 PR 6: the bank lives in the
    // settings database now; the file is read once as a frozen import source
    // when no document exists). Defaults to LocalMemoryStore::defaultFilePath().
    // Setting it drops the loaded state so the next load() re-evaluates.
    void setFilePath(const QString& path);
    QString filePath() const { return m_filePath; }

    // Read the bank from disk. Idempotent: a second call without an intervening
    // setFilePath() is a no-op, so reconnecting does not re-read the file (and
    // cannot lose an unsaved edit made while disconnected).
    void load();
    bool isLoaded() const { return m_loaded; }
    // False when the file could not be understood, so edits are refused and the
    // file is never replaced. Distinct from isLoaded(), which only says the read
    // was attempted — see load().
    bool isWritable() const { return m_writable; }

    const QMap<int, MemoryEntry>& entries() const { return m_entries; }

    // Handle one `memory …` command. Returns handled=false for anything outside
    // the four verbs above.
    CommandResult handleCommand(const QString& command);

    // RadioModel's post-decode view of a slot, and the authoritative one.
    void record(int index, const MemoryEntry& entry);
    void forget(int index);

    // Write now if anything is pending. Called on disconnect and at teardown so
    // the debounce window can never be the reason an edit is lost.
    void flush();

    // Last file-write failure, empty when the last save succeeded. Surfaced so a
    // read-only config dir shows up as something other than memories that
    // quietly fail to come back.
    QString lastError() const { return m_lastError; }

    // A bank this large is a runaway import, not an operator's channel list.
    // Refusing past it keeps a loop bug from growing the file without bound.
    static constexpr int kMaxSlots = 10000;

signals:
    // A save failed. Emitted once per failure, not once per debounce tick.
    void saveFailed(const QString& error);

private:
    int allocateSlot() const;
    void scheduleSave();
    // Concurrent-writer detection, database edition: the document's savedAt
    // stamp is snapshotted at load and after each of our own writes, and
    // compared before the next write. Best-effort, not a lock — see flush().
    void rememberDocumentState();
    bool foreignWriteDetected() const;

    QString m_filePath;
    QMap<int, MemoryEntry> m_entries;
    QTimer m_saveTimer;
    QString m_lastError;
    // The read has been attempted (latches; nothing re-reads).
    bool m_loaded{false};
    // The stored bank (document or legacy file) was understood, so replacing
    // it wholesale is safe. False for a bank this build cannot read — see
    // load(), which explains why these must be two flags and not one.
    bool m_writable{true};
    bool m_dirty{false};
    // The document's savedAt as of the last read or our own last write.
    QString m_seenSavedAt;
};

}  // namespace AetherSDR
