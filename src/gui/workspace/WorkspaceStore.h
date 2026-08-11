#pragma once

// Persistence for the workspace document (RFC #4887, phase 2).
//
// One key, one writer, one atomic whole-document write — Principle V and
// Principle XIV.  The document is station-scoped rather than radio-scoped: a
// workspace profile is the operator's arrangement, not a property of a radio.
// An operator who wants a different layout per rig makes a workspace per rig
// and binds it, which is a choice they get to make instead of one the storage
// layer makes for them.
//
// ── The auto-commit contract (RFC decision 7) ────────────────────────────
//
// There is no "save layout" step: placement changes commit as they happen, so
// an automatic workspace switch can never discard unsaved work, because there
// is none.  The cost is write volume — one drag is hundreds of geometry
// changes and every commit is a whole-document write by construction — so:
//
//   * touch() marks the document dirty and (re)starts a debounce timer;
//   * flush() writes immediately, and is called at the END of a gesture
//     (drag/resize release, add/remove/raise, workspace switch, shutdown);
//   * flushes are SUPPRESSED while restoring, because replaying saved state
//     would otherwise re-write exactly what it is reading.
//
// That is the contract ContainerManager already established for the same
// reason (ContainerManager.h:142-144, #4427); this follows it rather than
// inventing a second discipline for the same problem.

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

    // Reads the stored document, and ONLY IF THE KEY IS ABSENT builds
    // "Classic" from the legacy keys and writes it.
    //
    // A present-but-unusable document is left exactly as it is: this returns
    // false with lastError() set and writes nothing, so the newer document
    // survives for the build that understands it.  Phase 3 surfaces that
    // rather than silently starting from Classic.
    //
    // NOTE: nothing calls this yet.  Phase 2 ships the migration tested but
    // not wired — the first caller is phase 3, when there is a canvas to
    // consume the result.  Running it earlier would write a new key on every
    // install for a document nothing reads.
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

    // ── Auto-commit ──────────────────────────────────────────────────────
    void touch();                 // dirty + (re)start the debounce
    bool flush();                 // write now; false if suppressed or clean
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
    bool writeNow();

    WorkspaceDocument m_document;
    QTimer*     m_debounce{nullptr};
    QString     m_lastError;
    QStringList m_warnings;
    int         m_debounceMs{kDefaultDebounceMs};
    bool        m_dirty{false};
    bool        m_restoring{false};
    bool        m_loaded{false};
};

}  // namespace AetherSDR
