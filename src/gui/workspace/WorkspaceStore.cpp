#include "gui/workspace/WorkspaceStore.h"

#include "core/AppSettings.h"
#include "gui/workspace/WorkspaceMigration.h"

#include <QTimer>

namespace AetherSDR {

const QString WorkspaceStore::kSettingsKey = QStringLiteral("Workspaces");

WorkspaceStore::WorkspaceStore(QObject* parent)
    : QObject(parent)
    , m_debounce(new QTimer(this))
{
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(m_debounceMs);
    connect(m_debounce, &QTimer::timeout, this, [this] { flush(); });
}

WorkspaceStore::~WorkspaceStore()
{
    // A pending debounce at teardown is unsaved operator work.  Auto-commit
    // promises there is no such thing, so the last thing this object does is
    // make that true.
    if (m_dirty && !m_restoring && !m_writeBlocked) {
        writeNow();
    }
}

WorkspaceStore::LoadResult WorkspaceStore::loadWithStatus()
{
    m_warnings.clear();
    m_lastError.clear();

    const QByteArray blob =
        AppSettings::instance().stationValue(kSettingsKey).toString().toUtf8();

    if (blob.trimmed().isEmpty()) {
        m_loaded       = false;
        m_writeBlocked = false;
        m_lastError    = QStringLiteral("no workspace document stored");
        return LoadResult::Absent;
    }

    WorkspaceDocument doc;
    if (!WorkspaceDocument::fromStoredJson(blob, &doc, &m_lastError, &m_warnings)) {
        // Present and refused.  Arm the write block so nothing downstream can
        // commit over a document this build could not read — including one a
        // newer build wrote.
        m_loaded       = false;
        m_writeBlocked = true;
        return LoadResult::Unusable;
    }

    if (doc.isEmpty()) {
        // Structurally valid but carrying no workspace: equivalent to nothing
        // stored, and there is no operator state in it to lose.  Reporting it
        // as Loaded would leave loadOrMigrate() succeeding with no workspace
        // at all and never migrating (PR #4900 review, L4).
        m_loaded       = false;
        m_writeBlocked = false;
        m_lastError    = QStringLiteral("stored document contains no workspaces");
        return LoadResult::Absent;
    }

    m_document     = doc;
    m_loaded       = true;
    m_dirty        = false;
    m_writeBlocked = false;
    emit documentChanged();
    return LoadResult::Loaded;
}

bool WorkspaceStore::loadOrMigrate(const QStringList& knownAppletIds,
                                   const QStringList& panIds,
                                   bool* migrated)
{
    if (migrated) {
        *migrated = false;
    }

    const LoadResult result = loadWithStatus();
    if (result == LoadResult::Loaded) {
        return true;
    }
    if (result == LoadResult::Unusable) {
        // Present but refused — a newer schema, or damage this build cannot
        // read.  Writing Classic here would destroy it, and for the
        // newer-schema case that is precisely the loss WorkspaceDocument's
        // guard exists to prevent: downgrade once and every workspace the
        // newer build held would be gone (PR #4900 review).  Leave the
        // document alone and report; the caller decides what to tell the
        // operator.
        return false;
    }

    // The key is genuinely absent.  Build Classic from whatever the legacy
    // keys say and write it once.  The legacy keys are left in place: their
    // current owners keep writing them for a release, so a downgrade still
    // finds the state it expects.
    const LegacyLayoutState legacy = readLegacyLayoutState(knownAppletIds);
    m_document  = buildClassicDocument(legacy, panIds);
    m_loaded    = true;
    m_dirty     = true;
    m_lastError.clear();

    emit documentChanged();
    flush();

    if (migrated) {
        *migrated = true;
    }
    return true;
}

void WorkspaceStore::allowOverwrite()
{
    m_writeBlocked = false;
}

void WorkspaceStore::setDocument(const WorkspaceDocument& doc)
{
    m_document = doc;
    m_loaded   = true;
    emit documentChanged();
    touch();
}

void WorkspaceStore::touch()
{
    m_dirty = true;
    if (m_restoring) {
        return;   // no timer while replaying — flush() would refuse anyway
    }
    m_debounce->start(m_debounceMs);
}

WorkspaceStore::FlushResult WorkspaceStore::flushWithStatus()
{
    m_debounce->stop();

    // Order matters: "suppressed" outranks "clean" so a caller cannot read a
    // refusal as nothing-to-do (PR #4900 review, L5).
    if (m_restoring || m_writeBlocked) {
        return FlushResult::Suppressed;
    }
    if (!m_dirty) {
        return FlushResult::Clean;
    }
    return writeNow();
}

WorkspaceStore::FlushResult WorkspaceStore::writeNow()
{
    auto& settings = AppSettings::instance();

    // AppSettings::save() returns void and silently declines in a read-only
    // session (a settings DB written by a newer build), during a
    // reset-triggered shutdown, and before load().  Clearing m_dirty and
    // reporting success there would make this store's durability promise
    // false — and m_dirty/flushed() are what phase 3 will build on, so the
    // lie would propagate (PR #4900 review, M1).
    if (!settings.storeWritable()) {
        m_lastError = QStringLiteral("settings store is not accepting writes");
        return FlushResult::Failed;   // stays dirty, for a later retry
    }

    // One whole-document write IS the atomic update (Principle XIV): there is
    // no partial state a crash between two calls could leave behind, because
    // there is only ever one call.
    settings.setStationValue(kSettingsKey,
                             QString::fromUtf8(m_document.toStoredJson()));

    // setStationValue only mutates the in-memory map; a genuine commit has to
    // reach disk or an abnormal exit loses it — the same reasoning as
    // ContainerManager's flush point (#4427).
    settings.save();

    m_dirty = false;
    m_lastError.clear();
    emit flushed();
    return FlushResult::Wrote;
}

void WorkspaceStore::setRestoring(bool restoring)
{
    m_restoring = restoring;
    if (restoring) {
        m_debounce->stop();
    } else if (m_dirty) {
        // Deliberately no flush on the way out: restore leaves the store
        // exactly as dirty as it found it.  But the debounce HAS to resume —
        // touch() starts no timer while suppressed, so without this a change
        // made during the replay would wait for the next unrelated touch(),
        // an explicit flush(), or the destructor, and a crash skips the
        // destructor entirely (PR #4900 review).
        m_debounce->start(m_debounceMs);
    }
}

void WorkspaceStore::setDebounceMs(int ms)
{
    m_debounceMs = ms < 0 ? 0 : ms;
    m_debounce->setInterval(m_debounceMs);
}

}  // namespace AetherSDR
