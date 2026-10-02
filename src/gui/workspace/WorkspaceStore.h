#pragma once

// Persistence for the workspace document (RFC #4887): one key, one writer, one
// atomic whole-document write. Station-scoped, not radio-scoped; per-rig layouts
// are workspace bindings.
//
// The auto-commit contract (RFC decision 7): no "save layout" step, so a workspace
// switch never discards work. touch() marks dirty and debounces; flush() writes
// now and runs at the end of each gesture, switch and shutdown; flushes are
// suppressed while restoring. Same contract as ContainerManager (#4427).

#include "gui/workspace/WorkspaceDocument.h"

#include <QObject>
#include <QString>
#include <QStringList>

class QTimer;

namespace AetherSDR {

class WorkspaceStore : public QObject {
    Q_OBJECT

public:
    // The AppSettings station key holding the whole document.
    static const QString kSettingsKey;

    // Debounce window for touch().  Long enough that a drag coalesces into
    // one write, short enough that an abnormal exit loses at most this much.
    static constexpr int kDefaultDebounceMs = 750;

    explicit WorkspaceStore(QObject* parent = nullptr);
    ~WorkspaceStore() override;

    // ── Loading ──────────────────────────────────────────────────────────
    //
    // Why this is three outcomes and not a bool: "nothing stored" and "stored
    // but unusable" must never be confused.  Migrating over the second one
    // would rewrite a document this build refused to parse — including one a
    // NEWER build wrote — which turns the schema guard into a data-loss path
    // on a downgrade (PR #4900 review).
    enum class LoadResult {
        Loaded,     // a usable document is in hand
        Absent,     // the key is genuinely empty — safe to migrate into
        Unusable,   // present but refused: newer schema, corrupt, truncated
    };

    // Reads the stored document.  `error` says why on a non-Loaded result,
    // and parse warnings (repaired damage) come back through warnings().
    LoadResult loadWithStatus();

    // Convenience wrapper: true only for LoadResult::Loaded.
    bool load() { return loadWithStatus() == LoadResult::Loaded; }

    // Reads the stored document; only if the key is absent, builds "Classic" from
    // the legacy keys and writes it. A present-but-unusable document is left
    // untouched: returns false with lastError() set so a newer build's data survives.
    bool loadOrMigrate(const QStringList& knownAppletIds,
                       const QStringList& panIds,
                       bool* migrated = nullptr);

    // True when the store held a usable document at the last load.
    bool isLoaded() const { return m_loaded; }
    QString lastError() const { return m_lastError; }
    QStringList warnings() const { return m_warnings; }

    // ── The document ─────────────────────────────────────────────────────
    const WorkspaceDocument& document() const { return m_document; }

    // Replace the document and mark it dirty.  Does not write on its own —
    // the caller ends the gesture with flush(), or lets the debounce fire.
    void setDocument(const WorkspaceDocument& doc);

    // Overwrite protection: a LoadResult::Unusable (e.g. newer-schema) document arms
    // a write block so no setDocument()/touch() can commit a default over the row
    // this build couldn't read. Only allowOverwrite() clears it. Mirrors the
    // write-side guard on AppSettings::setRadioFeature() (#4614).
    bool isWriteBlocked() const { return m_writeBlocked; }

    // Deliberately discard the unreadable stored document and allow writes
    // again.  For a caller that has told the operator what is about to happen
    // and had it confirmed — never a default, and never automatic.
    void allowOverwrite();

    // Dirty + (re)start the debounce. A drag emits ~100 placement changes/s and
    // each commit is a whole-document write, so at kDefaultDebounceMs a continuous
    // drag costs ~1.3 writes/s and a finished gesture exactly one (a few KB each).
    void touch();                 // dirty + (re)start the debounce

    enum class FlushResult {
        Wrote,        // committed to AppSettings
        Clean,        // nothing pending
        Suppressed,   // restoring, or the write block is armed
        Failed,       // the settings store refused the write
    };

    // Write now, reporting which of the four happened.  Callers that only
    // care whether a write landed can use flush().
    FlushResult flushWithStatus();

    // True only for FlushResult::Wrote.
    bool flush() { return flushWithStatus() == FlushResult::Wrote; }

    bool isDirty() const { return m_dirty; }

    // Suppress flushes while saved state is being replayed.  Setting it back
    // to false does NOT flush: restore must leave the store exactly as clean
    // as it found it.
    void setRestoring(bool restoring);
    bool isRestoring() const { return m_restoring; }

    int debounceMs() const { return m_debounceMs; }
    void setDebounceMs(int ms);

signals:
    // Emitted after a write actually reached AppSettings.
    void flushed();
    // Emitted when the in-memory document is replaced (load, migrate, set).
    void documentChanged();

private:
    FlushResult writeNow();

    WorkspaceDocument m_document;
    QTimer*     m_debounce{nullptr};
    QString     m_lastError;
    QStringList m_warnings;
    int         m_debounceMs{kDefaultDebounceMs};
    bool        m_dirty{false};
    bool        m_restoring{false};
    bool        m_loaded{false};
    bool        m_writeBlocked{false};
};

}  // namespace AetherSDR
