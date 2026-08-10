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
    if (m_dirty && !m_restoring) {
        writeNow();
    }
}

bool WorkspaceStore::load()
{
    m_warnings.clear();
    m_lastError.clear();

    const QByteArray blob =
        AppSettings::instance().stationValue(kSettingsKey).toString().toUtf8();

    WorkspaceDocument doc;
    if (!WorkspaceDocument::fromStoredJson(blob, &doc, &m_lastError, &m_warnings)) {
        m_loaded = false;
        return false;
    }

    m_document = doc;
    m_loaded   = true;
    m_dirty    = false;
    emit documentChanged();
    return true;
}

bool WorkspaceStore::loadOrMigrate(const QStringList& knownAppletIds,
                                   const QStringList& panIds,
                                   bool* migrated)
{
    if (load()) {
        if (migrated) {
            *migrated = false;
        }
        return true;
    }

    // Nothing usable stored.  Build Classic from whatever the legacy keys
    // say and write it once.  The legacy keys are left in place: their
    // current owners keep writing them for a release, so a downgrade still
    // finds the state it expects.
    const LegacyLayoutState legacy = readLegacyLayoutState(knownAppletIds);
    m_document = buildClassicDocument(legacy, panIds);
    m_loaded   = true;
    m_dirty    = true;

    emit documentChanged();
    flush();

    if (migrated) {
        *migrated = true;
    }
    return true;
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

bool WorkspaceStore::flush()
{
    m_debounce->stop();

    if (m_restoring || !m_dirty) {
        return false;
    }
    return writeNow();
}

bool WorkspaceStore::writeNow()
{
    auto& settings = AppSettings::instance();

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
    emit flushed();
    return true;
}

void WorkspaceStore::setRestoring(bool restoring)
{
    m_restoring = restoring;
    if (restoring) {
        m_debounce->stop();
    }
    // Deliberately no flush on the way out: restore must leave the store
    // exactly as clean — or as dirty — as it found it.
}

void WorkspaceStore::setDebounceMs(int ms)
{
    m_debounceMs = ms < 0 ? 0 : ms;
    m_debounce->setInterval(m_debounceMs);
}

}  // namespace AetherSDR
