#include "SettingsDatabase.h"

#include <QDebug>
#include <QFile>

#include <sqlite3.h>

// Belt against a -DUSE_SYSTEM_SQLITE=ON pkg-config floor (CMakeLists.txt,
// sqlite3>=3.33) disagreeing with the header actually picked up at compile
// time: SettingsDatabase uses sqlite_schema (3.33.0, below) and VACUUM INTO
// (3.27.0). Catches the mismatch at compile time rather than at first launch.
static_assert(SQLITE_VERSION_NUMBER >= 3033000,
              "SettingsDatabase requires SQLite >= 3.33.0 (sqlite_schema)");

namespace AetherSDR {

namespace {

// Bound-parameter helper: all our text flows QString <-> UTF-8.
int bindText(sqlite3_stmt* stmt, int index, const QString& text)
{
    const QByteArray utf8 = text.toUtf8();
    return sqlite3_bind_text(stmt, index, utf8.constData(), utf8.size(),
                             SQLITE_TRANSIENT);
}

QString columnText(sqlite3_stmt* stmt, int index)
{
    const unsigned char* text = sqlite3_column_text(stmt, index);
    if (text == nullptr) {
        return {};
    }
    return QString::fromUtf8(reinterpret_cast<const char*>(text),
                             sqlite3_column_bytes(stmt, index));
}

// RAII for sqlite3_stmt so every early return finalizes.
class Statement {
public:
    Statement(sqlite3* db, const char* sql)
    {
        if (sqlite3_prepare_v2(db, sql, -1, &m_stmt, nullptr) != SQLITE_OK) {
            m_stmt = nullptr;
        }
    }
    ~Statement()
    {
        if (m_stmt != nullptr) {
            sqlite3_finalize(m_stmt);
        }
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    sqlite3_stmt* get() const { return m_stmt; }
    bool valid() const { return m_stmt != nullptr; }

private:
    sqlite3_stmt* m_stmt = nullptr;
};

bool setOwnerOnlyPermissions(const QString& filePath, bool required,
                             QString& error)
{
#ifdef Q_OS_WIN
    Q_UNUSED(filePath);
    Q_UNUSED(required);
    Q_UNUSED(error);
    return true;
#else
    if (!QFile::exists(filePath)) {
        if (!required) {
            return true;
        }
        error = QStringLiteral("SQLite did not create %1").arg(filePath);
        return false;
    }

    const QFileDevice::Permissions ownerOnly =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!QFile::setPermissions(filePath, ownerOnly)) {
        error = QStringLiteral("cannot set owner-only permissions on %1")
                    .arg(filePath);
        return false;
    }
    return true;
#endif
}

bool hardenDatabaseFilePermissions(const QString& path, QString& error)
{
    return setOwnerOnlyPermissions(path, true, error)
           && setOwnerOnlyPermissions(path + QStringLiteral("-wal"), false,
                                      error)
           && setOwnerOnlyPermissions(path + QStringLiteral("-shm"), false,
                                      error);
}

} // namespace

SettingsDatabase::SettingsDatabase() = default;

SettingsDatabase::~SettingsDatabase()
{
    close();
}

bool SettingsDatabase::exec(const char* sql)
{
    char* errMsg = nullptr;
    const int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        m_lastError = QString::fromUtf8(errMsg ? errMsg : "unknown sqlite error");
        sqlite3_free(errMsg);
        // Busy is contention, not corruption — open()'s callers must be able
        // to tell them apart wherever in the open sequence the busy landed
        // (under WAL a concurrent EXCLUSIVE first bites in createSchema's
        // BEGIN IMMEDIATE, not the read probe).
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            m_lastOpenBusy = true;
        }
        qWarning() << "SettingsDatabase: exec failed:" << sql << "—" << m_lastError;
        return false;
    }
    return true;
}

bool SettingsDatabase::open(const QString& path)
{
    close();
    m_newerSchema = false;
    m_lastOpenBusy = false;

    sqlite3* db = nullptr;
    const QByteArray utf8Path = path.toUtf8();
    if (sqlite3_open_v2(utf8Path.constData(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr)
        != SQLITE_OK) {
        m_lastError = db ? QString::fromUtf8(sqlite3_errmsg(db))
                         : QStringLiteral("out of memory");
        qWarning() << "SettingsDatabase: cannot open" << path << "—" << m_lastError;
        sqlite3_close(db);
        return false;
    }
    m_db = db;
    m_path = path;

    // Re-establish, at runtime, the two aether_sqlite3 compile-time hardening
    // options that a -DUSE_SYSTEM_SQLITE=ON distro library does not carry.
    // Both are cheap and safe to redo unconditionally on the vendored path,
    // keeping one behavior for both builds.
    const int dqsDmlResult =
        sqlite3_db_config(m_db, SQLITE_DBCONFIG_DQS_DML, 0, nullptr);
    const int dqsDdlResult =
        sqlite3_db_config(m_db, SQLITE_DBCONFIG_DQS_DDL, 0, nullptr);
    if (dqsDmlResult != SQLITE_OK || dqsDdlResult != SQLITE_OK) {
        m_lastError = QStringLiteral("cannot disable SQLite double-quoted "
                                     "string literals (%1/%2)")
                          .arg(dqsDmlResult)
                          .arg(dqsDdlResult);
        qWarning() << "SettingsDatabase:" << m_lastError;
        close();
        return false;
    }

    // busy_timeout first so a concurrent instance's transaction doesn't turn
    // every subsequent pragma into an immediate SQLITE_BUSY failure.
    sqlite3_busy_timeout(m_db, 5000);

    if (!exec("PRAGMA journal_mode=WAL;")
        || !exec("PRAGMA synchronous=NORMAL;")) {
        close();
        return false;
    }

    // sqlite3_open_v2() creates the handle lazily; journal_mode is the first
    // statement that guarantees the database file exists. Harden it before
    // createSchema() performs the first write. SQLite then derives new WAL/SHM
    // modes from the database mode on Unix; the later pass verifies all files
    // explicitly once the write has materialized them.
    if (!hardenDatabaseFilePermissions(path, m_lastError)) {
        qWarning() << "SettingsDatabase:" << m_lastError;
        close();
        return false;
    }

    // A non-database file (e.g. truncated or foreign) often only fails once a
    // real query touches it — probe before trusting it.
    {
        Statement probe(m_db, "SELECT count(*) FROM sqlite_schema;");
        const int rc = probe.valid() ? sqlite3_step(probe.get()) : SQLITE_ERROR;
        if (rc != SQLITE_ROW) {
            m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
            // SQLITE_BUSY past the timeout means another process holds the
            // store — healthy, just contended. Report it distinctly so the
            // caller fails the session instead of quarantining a live DB.
            m_lastOpenBusy = (rc == SQLITE_BUSY || rc == SQLITE_LOCKED);
            qWarning() << "SettingsDatabase:" << path
                       << (m_lastOpenBusy ? "is locked by another process —"
                                          : "is not a readable database —")
                       << m_lastError;
            close();
            return false;
        }
    }

    int userVersion = 0;
    {
        Statement stmt(m_db, "PRAGMA user_version;");
        if (stmt.valid() && sqlite3_step(stmt.get()) == SQLITE_ROW) {
            userVersion = sqlite3_column_int(stmt.get(), 0);
        }
    }

    if (userVersion > kSchemaVersion) {
        // Newer binary's database: reopen READ-ONLY so the ENGINE enforces
        // what the caller's read-only convention promises (PR #4612 review) —
        // and close without a checkpoint, which would write the newer file.
        if (!hardenDatabaseFilePermissions(path, m_lastError)) {
            qWarning() << "SettingsDatabase:" << m_lastError;
            close();
            return false;
        }
        m_newerSchema = true;
        sqlite3_close(m_db);
        m_db = nullptr;
        sqlite3* readOnlyDb = nullptr;
        if (sqlite3_open_v2(utf8Path.constData(), &readOnlyDb,
                            SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            m_lastError = readOnlyDb
                              ? QString::fromUtf8(sqlite3_errmsg(readOnlyDb))
                              : QStringLiteral("out of memory");
            sqlite3_close(readOnlyDb);
            m_path.clear();
            return false;
        }
        m_db = readOnlyDb;
        m_readOnly = true;
        sqlite3_busy_timeout(m_db, 5000);
        qWarning() << "SettingsDatabase: schema version" << userVersion
                   << "is newer than this binary's" << kSchemaVersion
                   << "— reopened read-only";
        return true;
    }

    if (!createSchema()) {
        close();
        return false;
    }
    if (!hardenDatabaseFilePermissions(path, m_lastError)) {
        qWarning() << "SettingsDatabase:" << m_lastError;
        close();
        return false;
    }
    return true;
}

bool SettingsDatabase::createSchema()
{
    if (!exec("BEGIN IMMEDIATE;")) {
        return false;
    }
    const bool ok =
        exec("CREATE TABLE IF NOT EXISTS meta ("
             "  key   TEXT PRIMARY KEY,"
             "  value TEXT NOT NULL"
             ") WITHOUT ROWID;")
        && exec("CREATE TABLE IF NOT EXISTS app_settings ("
                "  key   TEXT PRIMARY KEY,"
                "  value TEXT NOT NULL"
                ") WITHOUT ROWID;")
        && exec("CREATE TABLE IF NOT EXISTS station_settings ("
                "  station TEXT NOT NULL,"
                "  key     TEXT NOT NULL,"
                "  value   TEXT NOT NULL,"
                "  PRIMARY KEY (station, key)"
                ") WITHOUT ROWID;")
        && exec("CREATE TABLE IF NOT EXISTS radio_settings ("
                "  family         TEXT NOT NULL,"
                "  radio_id       TEXT NOT NULL,"
                "  feature        TEXT NOT NULL,"
                "  schema_version INTEGER NOT NULL,"
                "  value          TEXT NOT NULL,"
                "  PRIMARY KEY (family, radio_id, feature)"
                ") WITHOUT ROWID;")
        && exec("PRAGMA user_version = 1;");
    if (!ok) {
        exec("ROLLBACK;");
        return false;
    }
    return exec("COMMIT;");
}

void SettingsDatabase::close()
{
    if (m_db == nullptr) {
        m_readOnly = false;
        return;
    }
    // Fold the WAL back into the main file so the on-disk .db is complete on
    // its own — Reset Settings and backup tooling depend on this. Never on a
    // read-only handle: a newer binary's file is not ours to write.
    if (!m_readOnly) {
        sqlite3_wal_checkpoint_v2(m_db, nullptr, SQLITE_CHECKPOINT_TRUNCATE,
                                  nullptr, nullptr);
    }
    sqlite3_close(m_db);
    m_db = nullptr;
    m_path.clear();
    m_readOnly = false;
}

bool SettingsDatabase::quickCheck()
{
    Statement stmt(m_db, "PRAGMA quick_check;");
    if (!stmt.valid() || sqlite3_step(stmt.get()) != SQLITE_ROW) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    return columnText(stmt.get(), 0) == QStringLiteral("ok");
}

bool SettingsDatabase::integrityCheck()
{
    Statement stmt(m_db, "PRAGMA integrity_check;");
    if (!stmt.valid() || sqlite3_step(stmt.get()) != SQLITE_ROW) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    return columnText(stmt.get(), 0) == QStringLiteral("ok");
}

QString SettingsDatabase::metaValue(const QString& key, const QString& defaultValue)
{
    Statement stmt(m_db, "SELECT value FROM meta WHERE key = ?1;");
    if (!stmt.valid()) {
        return defaultValue;
    }
    bindText(stmt.get(), 1, key);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        return columnText(stmt.get(), 0);
    }
    return defaultValue;
}

bool SettingsDatabase::setMetaValue(const QString& key, const QString& value)
{
    Statement stmt(m_db,
        "INSERT INTO meta (key, value) VALUES (?1, ?2) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;");
    if (!stmt.valid()) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    bindText(stmt.get(), 1, key);
    bindText(stmt.get(), 2, value);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    return true;
}

bool SettingsDatabase::loadAppSettings(QMap<QString, QString>& out)
{
    Statement stmt(m_db, "SELECT key, value FROM app_settings;");
    if (!stmt.valid()) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    int rc = 0;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        out.insert(columnText(stmt.get(), 0), columnText(stmt.get(), 1));
    }
    if (rc != SQLITE_DONE) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    return true;
}

bool SettingsDatabase::loadStationSettings(const QString& station,
                                           QMap<QString, QString>& out)
{
    Statement stmt(m_db,
        "SELECT key, value FROM station_settings WHERE station = ?1;");
    if (!stmt.valid()) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    bindText(stmt.get(), 1, station);
    int rc = 0;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        out.insert(columnText(stmt.get(), 0), columnText(stmt.get(), 1));
    }
    if (rc != SQLITE_DONE) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    return true;
}

bool SettingsDatabase::upsertApp(const QString& key, const QString& value)
{
    Statement stmt(m_db,
        "INSERT INTO app_settings (key, value) VALUES (?1, ?2) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;");
    if (!stmt.valid()) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    bindText(stmt.get(), 1, key);
    bindText(stmt.get(), 2, value);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    return true;
}

bool SettingsDatabase::removeApp(const QString& key)
{
    Statement stmt(m_db, "DELETE FROM app_settings WHERE key = ?1;");
    if (!stmt.valid()) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    bindText(stmt.get(), 1, key);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    return true;
}

bool SettingsDatabase::upsertStation(const QString& station, const QString& key,
                                     const QString& value)
{
    Statement stmt(m_db,
        "INSERT INTO station_settings (station, key, value) VALUES (?1, ?2, ?3) "
        "ON CONFLICT(station, key) DO UPDATE SET value = excluded.value;");
    if (!stmt.valid()) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    bindText(stmt.get(), 1, station);
    bindText(stmt.get(), 2, key);
    bindText(stmt.get(), 3, value);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    return true;
}

bool SettingsDatabase::removeStation(const QString& station, const QString& key)
{
    Statement stmt(m_db,
        "DELETE FROM station_settings WHERE station = ?1 AND key = ?2;");
    if (!stmt.valid()) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    bindText(stmt.get(), 1, station);
    bindText(stmt.get(), 2, key);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    return true;
}

bool SettingsDatabase::removeStationAll(const QString& station)
{
    Statement stmt(m_db, "DELETE FROM station_settings WHERE station = ?1;");
    if (!stmt.valid()) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    bindText(stmt.get(), 1, station);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    return true;
}

bool SettingsDatabase::readApp(const QString& key, QString& value)
{
    Statement stmt(m_db, "SELECT value FROM app_settings WHERE key = ?1;");
    if (!stmt.valid()) {
        return false;
    }
    bindText(stmt.get(), 1, key);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return false;
    }
    value = columnText(stmt.get(), 0);
    return true;
}

qint64 SettingsDatabase::appCount()
{
    Statement stmt(m_db, "SELECT count(*) FROM app_settings;");
    if (!stmt.valid() || sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return -1;
    }
    return sqlite3_column_int64(stmt.get(), 0);
}

bool SettingsDatabase::upsertRadioFeature(const QString& family,
                                          const QString& radioId,
                                          const QString& feature,
                                          int schemaVersion,
                                          const QString& value)
{
    Statement stmt(m_db,
        "INSERT INTO radio_settings (family, radio_id, feature, schema_version, value) "
        "VALUES (?1, ?2, ?3, ?4, ?5) "
        "ON CONFLICT(family, radio_id, feature) DO UPDATE SET "
        "schema_version = excluded.schema_version, value = excluded.value;");
    if (!stmt.valid()) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    bindText(stmt.get(), 1, family);
    bindText(stmt.get(), 2, radioId);
    bindText(stmt.get(), 3, feature);
    sqlite3_bind_int(stmt.get(), 4, schemaVersion);
    bindText(stmt.get(), 5, value);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    return true;
}

bool SettingsDatabase::readRadioFeature(const QString& family,
                                        const QString& radioId,
                                        const QString& feature,
                                        int& schemaVersion, QString& value)
{
    Statement stmt(m_db,
        "SELECT schema_version, value FROM radio_settings "
        "WHERE family = ?1 AND radio_id = ?2 AND feature = ?3;");
    if (!stmt.valid()) {
        return false;
    }
    bindText(stmt.get(), 1, family);
    bindText(stmt.get(), 2, radioId);
    bindText(stmt.get(), 3, feature);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return false;
    }
    schemaVersion = sqlite3_column_int(stmt.get(), 0);
    value = columnText(stmt.get(), 1);
    return true;
}

bool SettingsDatabase::removeRadioFeature(const QString& family,
                                          const QString& radioId,
                                          const QString& feature)
{
    Statement stmt(m_db,
        "DELETE FROM radio_settings "
        "WHERE family = ?1 AND radio_id = ?2 AND feature = ?3;");
    if (!stmt.valid()) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    bindText(stmt.get(), 1, family);
    bindText(stmt.get(), 2, radioId);
    bindText(stmt.get(), 3, feature);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    return true;
}

bool SettingsDatabase::listRadioFeatures(QList<RadioFeatureRow>& out)
{
    Statement stmt(m_db,
        "SELECT family, radio_id, feature, schema_version, value "
        "FROM radio_settings ORDER BY family, radio_id, feature;");
    if (!stmt.valid()) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    int rc = 0;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        RadioFeatureRow row;
        row.family = columnText(stmt.get(), 0);
        row.radioId = columnText(stmt.get(), 1);
        row.feature = columnText(stmt.get(), 2);
        row.schemaVersion = sqlite3_column_int(stmt.get(), 3);
        row.value = columnText(stmt.get(), 4);
        out.append(row);
    }
    if (rc != SQLITE_DONE) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    return true;
}

bool SettingsDatabase::beginExclusive() { return exec("BEGIN EXCLUSIVE;"); }
bool SettingsDatabase::begin()          { return exec("BEGIN IMMEDIATE;"); }
bool SettingsDatabase::commit()         { return exec("COMMIT;"); }
bool SettingsDatabase::rollback()       { return exec("ROLLBACK;"); }

bool SettingsDatabase::backupTo(const QString& destPath)
{
    // VACUUM INTO refuses to overwrite; a stale partial from an interrupted
    // earlier attempt must not block the backup forever.
    QFile::remove(destPath);

    Statement stmt(m_db, "VACUUM INTO ?1;");
    if (!stmt.valid()) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        return false;
    }
    bindText(stmt.get(), 1, destPath);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        m_lastError = QString::fromUtf8(sqlite3_errmsg(m_db));
        qWarning() << "SettingsDatabase: backup to" << destPath
                   << "failed —" << m_lastError;
        QFile::remove(destPath);
        return false;
    }
    QFile::setPermissions(destPath,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

bool SettingsDatabase::verifyBackupFile(const QString& path)
{
    sqlite3* db = nullptr;
    const QByteArray utf8Path = path.toUtf8();
    if (sqlite3_open_v2(utf8Path.constData(), &db, SQLITE_OPEN_READONLY, nullptr)
        != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    bool ok = false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA quick_check;", -1, &stmt, nullptr)
            == SQLITE_OK
        && sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(stmt, 0);
        ok = text != nullptr
             && qstrcmp(reinterpret_cast<const char*>(text), "ok") == 0;
    }
    sqlite3_finalize(stmt);
    stmt = nullptr;
    if (ok) {
        ok = sqlite3_prepare_v2(db, "SELECT count(*) FROM app_settings;", -1,
                                &stmt, nullptr) == SQLITE_OK
             && sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return ok;
}

bool SettingsDatabase::readAppValueFromFile(const QString& path,
                                            const QString& key, QString& value)
{
    sqlite3* db = nullptr;
    const QByteArray utf8Path = path.toUtf8();
    if (sqlite3_open_v2(utf8Path.constData(), &db, SQLITE_OPEN_READONLY, nullptr)
        != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    sqlite3_busy_timeout(db, 2000);
    bool found = false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT value FROM app_settings WHERE key = ?1;",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        const QByteArray utf8Key = key.toUtf8();
        sqlite3_bind_text(stmt, 1, utf8Key.constData(), utf8Key.size(),
                          SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            value = columnText(stmt, 0);
            found = true;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return found;
}

bool SettingsDatabase::readStationValueFromFile(const QString& path,
                                                const QString& station,
                                                const QString& key,
                                                QString& value)
{
    sqlite3* db = nullptr;
    const QByteArray utf8Path = path.toUtf8();
    if (sqlite3_open_v2(utf8Path.constData(), &db, SQLITE_OPEN_READONLY, nullptr)
        != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    sqlite3_busy_timeout(db, 2000);
    bool found = false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db,
            "SELECT value FROM station_settings WHERE station = ?1 AND key = ?2;",
            -1, &stmt, nullptr)
        == SQLITE_OK) {
        const QByteArray utf8Station = station.toUtf8();
        const QByteArray utf8Key = key.toUtf8();
        sqlite3_bind_text(stmt, 1, utf8Station.constData(), utf8Station.size(),
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, utf8Key.constData(), utf8Key.size(),
                          SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            value = columnText(stmt, 0);
            found = true;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return found;
}

} // namespace AetherSDR
