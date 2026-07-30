#pragma once

#include <QMap>
#include <QMutex>
#include <QReadWriteLock>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <memory>

class QLockFile;

namespace AetherSDR {

class SettingsDatabase;

// Client settings store, backed by SQLite (RFC #4603) at
// <config>/AetherSDR/AetherSDR.db — ~/.config on Linux, ~/Library/Preferences
// on macOS, %LOCALAPPDATA% on Windows. Values are strings; booleans are the
// literal strings "True"/"False" (unchanged convention).
//
// Usage (unchanged since the XML era):
//   auto& s = AppSettings::instance();
//   s.setValue("LastConnectedRadioSerial", "2125-1213-8600-8895");
//   QString serial = s.value("LastConnectedRadioSerial").toString();
//   s.save();
//
// Reads and writes hit an in-memory cache; save() commits the dirty rows in
// one transaction. All public accessors are thread-safe (the XML store was
// not — #4602); never hold the audio callback on save(), which does I/O.
//
// On first launch after the upgrade, load() imports the legacy XML store in
// one verified transaction and leaves the XML in place as a frozen snapshot
// (downgrade insurance). Credentials are never imported into the database —
// they divert to QtKeychain / the session vault (RFC #4603 proposal E).
class AppSettings {
public:
    static AppSettings& instance();

    // Load settings from disk. Called once at startup. Performs the one-time
    // XML -> SQLite import, integrity checks, and corruption recovery.
    void load();

    // Commit dirty keys to the database in one transaction.
    void save();

    // Get/set top-level settings.
    QVariant value(const QString& key, const QVariant& defaultValue = {}) const;
    void setValue(const QString& key, const QVariant& val);
    void remove(const QString& key);
    bool contains(const QString& key) const;

    // Per-station settings (rows in station_settings keyed by station name).
    QVariant stationValue(const QString& key, const QVariant& defaultValue = {}) const;
    void setStationValue(const QString& key, const QVariant& val);

    // Station name (defaults to "AetherSDR").
    QString stationName() const;
    void setStationName(const QString& name);

    // Key enumeration for diagnostics and the --config CLI (the settings
    // browser of RFC #4603 proposal D). Not for feature code — features own
    // their keys and never iterate the shared namespace.
    QStringList allKeysForDiagnostics() const;
    QStringList stationKeysForDiagnostics() const;

    // Path of the settings database (the store this class persists to).
    QString filePath() const { return m_filePath; }
    // The frozen pre-SQLite XML snapshot (may not exist).
    QString legacyXmlPath() const;

    // Human-readable notice from load() when something a user should know
    // about happened (backup restored, newer schema opened read-only, changes
    // made by an older version not carried forward). Empty when clean.
    QString loadNotice() const;

    // ── Session credential vault ─────────────────────────────────────────────
    // Credentials are NEVER stored in the database. When the XML import (or a
    // no-keychain build) encounters one, it lands here — in memory, for this
    // process only. Features consume via take (one-shot) after which they own
    // writing it to the OS keychain.
    QString takeSessionCredential(const QString& keychainKey);
    void setSessionCredential(const QString& keychainKey, const QString& value);

    // GUI-client identity is persistent for ordinary reconnect/session restore,
    // but must be distinct for concurrently-running processes and automation
    // worktrees. Call once after load(), before constructing MainWindow.
    void initializeGuiClientIdentity();
    QString effectiveGuiClientId() const;
    QString persistentGuiClientId() const;
    QString effectiveStationName() const;
    QString automationIdentity() const;
    QString automationAgentName() const;
    bool guiClientIdentityIsTransient() const;
    bool rotatePersistentGuiClientId(const QString& reason);
    bool resolveLiveGuiClientIdCollision(const QString& otherStation);
    void recordPersistentGuiClientIdReply(const QString& clientId);

    // Clear all loaded settings from memory without writing anything back out.
    void reset();

    // Migrate from old QSettings (INI format) on a genuine first launch.
    void migrateFromQSettings();

    // Checkpoint + close the database and write a bounded pre-reset backup.
    // Reset Settings must call this before deleting store files (Windows holds
    // the files locked while a connection is open). Returns the backup path,
    // or an empty string if the backup could not be written.
    QString shutdownForReset();

private:
    enum class LoadState {
        NotAttempted,
        ReadyToSave,
        ReadOnly,   // newer-schema database — reads work, writes refused
        Failed,
    };

    AppSettings();
    ~AppSettings();
    AppSettings(const AppSettings&) = delete;
    AppSettings& operator=(const AppSettings&) = delete;

    // One-time path migration for the legacy XML file (Windows/macOS
    // triple-nesting). Still needed: the import reads the XML from the
    // canonical location.
    void migrateSettingsPath();

    // XML -> SQLite import. Returns true when the database was stamped.
    bool importLegacyXml(QMap<QString, QString> settings,
                         QMap<QString, QString> stationSettings,
                         const QString& stationName,
                         const QString& sourcePath);
    // Divert every known credential out of the parsed XML into the session
    // vault (and on to the keychain) so it never reaches the database.
    void extractCredentials(QMap<QString, QString>& settings);
    void persistVaultToKeychain();

    void quarantineCorruptStore();
    bool restoreNewestVerifiedBackup();
    QString writeRollingBackup(const QString& tag);
    void pruneBackups(const QString& tag, int keep);

    // Populate the cache from an open, stamped database.
    bool loadFromDatabase();

    QString m_filePath;                          // database path
    std::unique_ptr<SettingsDatabase> m_db;

    mutable QReadWriteLock m_lock;               // guards every field below
    QMap<QString, QString> m_settings;           // top-level key=value cache
    QMap<QString, QString> m_stationSettings;    // per-station key=value cache
    QSet<QString> m_dirtyKeys;
    QSet<QString> m_dirtyStationKeys;
    QString m_stationName{"AetherSDR"};
    QString m_stationRenamedFrom;                // pending station-row move
    QMap<QString, QString> m_sessionCredentials; // keychain key -> secret
    QString m_loadNotice;
    LoadState m_loadState{LoadState::NotAttempted};

    QMutex m_saveMutex;                          // serializes save()/import I/O

    std::unique_ptr<QLockFile> m_guiClientLock;
    QString m_persistentGuiClientId;
    QString m_effectiveGuiClientId;
    QString m_effectiveStationName;
    QString m_automationIdentity;
    QString m_automationAgentName;
    bool m_guiClientIdentityInitialized{false};
    bool m_guiClientIdentityTransient{false};
};

} // namespace AetherSDR
