#pragma once

#include <QString>
#include <QList>
#include <QMap>

struct sqlite3;

namespace AetherSDR {

// Thin RAII wrapper around the vendored SQLite amalgamation for the client
// settings store (RFC #4603). This is the ONLY translation unit in the tree
// permitted to include sqlite3.h — everything else goes through AppSettings
// or this class, so the compile-option surface and any future engine swap
// stay single-point.
//
// Schema v1 (all tables created up front so the file format is stable from
// the first release, even though radio_settings is consumed starting PR 2):
//
//   meta             (key TEXT PRIMARY KEY, value TEXT NOT NULL)
//   app_settings     (key TEXT PRIMARY KEY, value TEXT NOT NULL)
//   station_settings (station, key, value; PRIMARY KEY (station, key))
//   radio_settings   (family, radio_id, feature, schema_version, value;
//                     PRIMARY KEY (family, radio_id, feature))
//
// Concurrency: SQLITE_THREADSAFE=1 (serialized) makes individual calls safe
// from any thread, but callers own transaction composition — AppSettings
// serializes its save path with its own mutex.
//
// Error handling: no exceptions (project style). Every method returns
// success/failure and logs via qWarning(); lastError() carries the most
// recent sqlite message for callers that surface errors to the user.
class SettingsDatabase {
public:
    static constexpr int kSchemaVersion = 1;

    SettingsDatabase();
    ~SettingsDatabase();
    SettingsDatabase(const SettingsDatabase&) = delete;
    SettingsDatabase& operator=(const SettingsDatabase&) = delete;

    // Open (creating if absent) with WAL + NORMAL sync + busy_timeout, and
    // create the v1 schema on a fresh file. Returns false on any failure —
    // including a file that is not a SQLite database. A database whose
    // user_version is NEWER than kSchemaVersion opens successfully but
    // reports isNewerSchema(); callers must treat it read-only and never
    // "repair" it.
    bool open(const QString& path);
    void close();                       // checkpoints WAL, then closes
    bool isOpen() const { return m_db != nullptr; }
    bool isNewerSchema() const { return m_newerSchema; }
    // True when the last failed open() was a lock/busy condition rather than
    // corruption — a busy database is HEALTHY and must never be quarantined
    // (PR #4612 review: POSIX rename() succeeds on open files, so quarantining
    // a busy store split-brains a concurrent instance's committed writes).
    bool lastOpenWasBusy() const { return m_lastOpenBusy; }
    QString path() const { return m_path; }
    QString lastError() const { return m_lastError; }

    // Integrity: cheap check for every startup; full check when cheap fails.
    bool quickCheck();
    bool integrityCheck();

    // meta table -------------------------------------------------------------
    QString metaValue(const QString& key, const QString& defaultValue = {});
    bool setMetaValue(const QString& key, const QString& value);

    // Bulk load --------------------------------------------------------------
    bool loadAppSettings(QMap<QString, QString>& out);
    // Loads only rows for `station`.
    bool loadStationSettings(const QString& station, QMap<QString, QString>& out);

    // Row mutation — call inside a transaction for multi-row updates ---------
    bool upsertApp(const QString& key, const QString& value);
    bool removeApp(const QString& key);
    bool upsertStation(const QString& station, const QString& key, const QString& value);
    bool removeStation(const QString& station, const QString& key);
    bool removeStationAll(const QString& station);

    // Single-row read (used by import verification).
    // Returns true and fills `value` iff the row exists.
    bool readApp(const QString& key, QString& value);
    qint64 appCount();

    // radio_settings — one versioned feature document per (family, radio, feature)
    // (RFC #4603 proposal A; radio_id "" = family-wide default row) -----------
    bool upsertRadioFeature(const QString& family, const QString& radioId,
                            const QString& feature, int schemaVersion,
                            const QString& value);
    bool readRadioFeature(const QString& family, const QString& radioId,
                          const QString& feature, int& schemaVersion,
                          QString& value);
    bool removeRadioFeature(const QString& family, const QString& radioId,
                            const QString& feature);
    // Full enumeration for diagnostics (--config features / support bundle).
    struct RadioFeatureRow {
        QString family;
        QString radioId;
        QString feature;
        int schemaVersion = 0;
        QString value;
    };
    bool listRadioFeatures(QList<RadioFeatureRow>& out);

    // Transactions -----------------------------------------------------------
    bool beginExclusive();   // BEGIN EXCLUSIVE — blocks concurrent writers
    bool begin();            // BEGIN IMMEDIATE
    bool commit();
    bool rollback();

    // Backups ----------------------------------------------------------------
    // VACUUM INTO a fresh single-file snapshot (safe against a live WAL DB,
    // unlike a file copy). Removes a pre-existing file at `destPath` first.
    bool backupTo(const QString& destPath);
    // Opens `path` read-only, runs quick_check, and confirms the app_settings
    // table exists. Static so a backup can be verified without disturbing the
    // live connection.
    static bool verifyBackupFile(const QString& path);

    // One-shot read-only fetch of a single app_settings row, without creating
    // the file or requiring a QCoreApplication — the pre-QApplication
    // bootstrap path (SettingsBootstrap). Returns true iff the row exists.
    static bool readAppValueFromFile(const QString& path, const QString& key,
                                     QString& value);

private:
    bool exec(const char* sql);
    bool createSchema();

    sqlite3* m_db = nullptr;
    QString m_path;
    QString m_lastError;
    bool m_newerSchema = false;
    bool m_readOnly = false;
    bool m_lastOpenBusy = false;
};

} // namespace AetherSDR
