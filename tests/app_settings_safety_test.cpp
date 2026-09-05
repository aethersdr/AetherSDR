// Safety suite for the SQLite-backed settings store (RFC #4603).
//
// Each scenario runs in its OWN PROCESS (see the CMake foreach) because
// AppSettings is a process-wide singleton and load state must not leak
// between cases. The suite covers the storage guarantees:
//   - save() before a successful load() can never create or damage a store
//   - the one-time XML import: parity, recovery-ladder sources, frozen
//     snapshot, meta stamping, credential exodus, no re-import
//   - corruption handling: quarantine + backup restore, XML re-import
//   - newer-schema databases open read-only
//   - dirty-row saves persist exactly the mutated rows
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/SettingsDatabase.h"
#include "core/SettingsPaths.h"
#include "core/SettingsSanitizer.h"
#include "core/backends/hl2/Hl2Discovery.h"
#include "gui/DisplaySettings.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QStandardPaths>
#include <QXmlStreamWriter>

#include <cstdio>

#ifndef Q_OS_WIN
#include <sys/stat.h>
#endif

using namespace AetherSDR;

namespace {

int g_failures = 0;

void expect(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++g_failures;
    }
}

QString xmlPath()
{
    return SettingsPaths::legacyXmlPath();
}

QString dbPath()
{
    return SettingsPaths::databasePath();
}

QByteArray settingsDocument(int count, const QString& valuePrefix)
{
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    QXmlStreamWriter xml(&buffer);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("Settings"));
    for (int index = 0; index < count; ++index) {
        xml.writeTextElement(
            QStringLiteral("Key%1").arg(index, 3, 10, QLatin1Char('0')),
            QStringLiteral("%1-%2").arg(valuePrefix).arg(index));
    }
    xml.writeEndElement();
    xml.writeEndDocument();
    buffer.close();
    return data;
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(data) == data.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

// Read one row straight from the database FILE — the persisted truth, not the
// in-memory cache.
bool dbRow(const QString& key, QString& value)
{
    return SettingsDatabase::readAppValueFromFile(dbPath(), key, value);
}

bool dbHas(const QString& key)
{
    QString ignored;
    return dbRow(key, ignored);
}

// ── Scenarios ────────────────────────────────────────────────────────────────

void testSaveBeforeLoad()
{
    const QByteArray live = settingsDocument(50, QStringLiteral("live"));
    expect(writeFile(xmlPath(), live), "production-like live fixture is written");

    AppSettings& settings = AppSettings::instance();
    settings.setValue(QStringLiteral("OnlyNewKey"), QStringLiteral("new-value"));
    settings.save();

    expect(!QFile::exists(dbPath()),
           "save-before-load creates no settings database");
    expect(readFile(xmlPath()) == live,
           "save-before-load leaves the legacy XML byte-for-byte unchanged");
}

void testXmlImportParity()
{
    // A fixture with a per-station section, so the import's station rows get
    // the same verification as app rows (PR #4612 review, K5PTB finding 3).
    QByteArray live;
    {
        QBuffer buffer(&live);
        buffer.open(QIODevice::WriteOnly);
        QXmlStreamWriter xml(&buffer);
        xml.setAutoFormatting(true);
        xml.writeStartDocument();
        xml.writeStartElement(QStringLiteral("Settings"));
        for (int index = 0; index < 50; ++index) {
            xml.writeTextElement(
                QStringLiteral("Key%1").arg(index, 3, 10, QLatin1Char('0')),
                QStringLiteral("original-%1").arg(index));
        }
        xml.writeTextElement(QStringLiteral("StationName"),
                             QStringLiteral("TestShack"));
        xml.writeStartElement(QStringLiteral("TestShack"));
        xml.writeTextElement(QStringLiteral("MeterSel"), QStringLiteral("S-Meter"));
        xml.writeEndElement();
        xml.writeEndElement();
        xml.writeEndDocument();
    }
    expect(writeFile(xmlPath(), live), "import fixture is written");

    AppSettings& settings = AppSettings::instance();
    settings.load();

    expect(settings.stationName() == QStringLiteral("TestShack")
               && settings.stationValue(QStringLiteral("MeterSel")).toString()
                      == QStringLiteral("S-Meter"),
           "the station section imports and is readback-verified");

    expect(settings.value(QStringLiteral("Key049")).toString()
               == QStringLiteral("original-49"),
           "the imported profile is fully readable");

    bool parity = true;
    for (int index = 0; index < 50; ++index) {
        QString value;
        parity = parity
                 && dbRow(QStringLiteral("Key%1").arg(index, 3, 10, QLatin1Char('0')),
                          value)
                 && value == QStringLiteral("original-%1").arg(index);
    }
    expect(parity, "every XML key round-trips into a database row verbatim");
    expect(readFile(xmlPath()) == live,
           "the XML stays in place as a byte-identical frozen snapshot");

    // Post-import state: mutations persist to the DB, never the XML.
    settings.setValue(QStringLiteral("AddedKey"), QStringLiteral("preserved"));
    settings.save();
    QString added;
    expect(dbRow(QStringLiteral("AddedKey"), added)
               && added == QStringLiteral("preserved"),
           "a post-import save lands in the database");
    expect(readFile(xmlPath()) == live,
           "a post-import save never touches the frozen XML");

    // A verified backup is written immediately after migration.
    const QDir backups(SettingsPaths::backupsDir());
    const QStringList backupFiles =
        backups.entryList({QStringLiteral("*-postmigration.db")}, QDir::Files);
    expect(backupFiles.size() == 1
               && SettingsDatabase::verifyBackupFile(
                      backups.filePath(backupFiles.value(0))),
           "a verified post-migration backup exists");
}

void testFirstRunInitialization()
{
    AppSettings& settings = AppSettings::instance();
    settings.load();

    expect(QFile::exists(dbPath()), "first-run load creates the database");
    expect(!QFile::exists(xmlPath()),
           "first-run initialization never creates a legacy XML file");
    QString autoConnect;
    QString lastRadio;
    expect(dbRow(QStringLiteral("AutoConnect"), autoConnect)
               && autoConnect == QStringLiteral("True")
               && dbRow(QStringLiteral("AutoConnectToLastRadio"), lastRadio)
               && lastRadio == QStringLiteral("True")
               && dbHas(QStringLiteral("GUIClientID")),
           "first-run initialization writes the intentional default profile");
}

// Pins the runtime hardening SettingsDatabase::open() re-establishes for
// -DUSE_SYSTEM_SQLITE=ON, where the distro library doesn't carry the
// vendored target's SQLITE_DEFAULT_FILE_PERMISSIONS=0600. Platform-gated
// because Windows ACLs have no POSIX owner-only bit for QFileDevice
// permissions to model.
void testDatabaseFilePermissions()
{
#ifdef Q_OS_WIN
    std::printf("[SKIP] database file is owner-only on disk (no POSIX mode bits on Windows)\n");
#else
    AppSettings& settings = AppSettings::instance();
    settings.load();

    const auto isOwnerOnly = [](const QString& path) {
        struct stat fileStatus {};
        const QByteArray nativePath = QFile::encodeName(path);
        return ::stat(nativePath.constData(), &fileStatus) == 0
               && (fileStatus.st_mode & 0777) == 0600;
    };

    expect(QFile::exists(dbPath()), "first-run load creates the database");
    expect(isOwnerOnly(dbPath()),
           "the database file is owner-read/write only, group/other have no access");

    // journal_mode=WAL is on by default (testFirstRunInitialization's own
    // profile confirms this store reaches createSchema()), so -wal exists
    // by the time load() returns; -shm's presence is backend-dependent and
    // only checked if SettingsDatabase actually created one.
    const QString walPath = dbPath() + QStringLiteral("-wal");
    if (QFile::exists(walPath)) {
        expect(isOwnerOnly(walPath),
               "the -wal sidecar is owner-read/write only");
    }
    const QString shmPath = dbPath() + QStringLiteral("-shm");
    if (QFile::exists(shmPath)) {
        expect(isOwnerOnly(shmPath),
               "the -shm sidecar is owner-read/write only");
    }
#endif
}

void testImportPromotesValidTemp()
{
    const QByteArray pending = settingsDocument(50, QStringLiteral("pending"));
    const QByteArray backup = settingsDocument(40, QStringLiteral("backup"));
    expect(writeFile(xmlPath() + QStringLiteral(".tmp"), pending),
           "interrupted-commit temporary fixture is written");
    expect(writeFile(xmlPath() + QStringLiteral(".bak"), backup),
           "interrupted-commit backup fixture is written");

    AppSettings& settings = AppSettings::instance();
    settings.load();

    expect(settings.value(QStringLiteral("Key049")).toString()
               == QStringLiteral("pending-49"),
           "a valid pending XML commit wins when the live XML is missing");
    QString value;
    expect(dbRow(QStringLiteral("Key049"), value)
               && value == QStringLiteral("pending-49"),
           "the pending commit's state is what reaches the database");
    expect(readFile(xmlPath() + QStringLiteral(".tmp")) == pending
               && readFile(xmlPath() + QStringLiteral(".bak")) == backup,
           "the import leaves every XML-era recovery artifact untouched");
}

void testImportFallsBackToBackup()
{
    const QByteArray corruptLive("<Settings><Broken>partial");
    const QByteArray goodBackup = settingsDocument(50, QStringLiteral("backup"));
    expect(writeFile(xmlPath(), corruptLive), "corrupt live fixture is written");
    expect(writeFile(xmlPath() + QStringLiteral(".bak"), goodBackup),
           "valid recovery fixture is written");

    AppSettings& settings = AppSettings::instance();
    settings.load();

    expect(settings.value(QStringLiteral("Key049")).toString()
               == QStringLiteral("backup-49"),
           "a corrupt live XML imports from its valid backup");
    expect(readFile(xmlPath()) == corruptLive,
           "the corrupt live XML is preserved for inspection");
}

void testUnusableArtifactsFailClosed()
{
    const QByteArray corruptLive("<Settings><Broken>partial");
    const QByteArray corruptBackup("<Settings><AlsoBroken>");
    expect(writeFile(xmlPath(), corruptLive), "corrupt live fixture is written");
    expect(writeFile(xmlPath() + QStringLiteral(".bak"), corruptBackup),
           "corrupt backup fixture is written");

    AppSettings& settings = AppSettings::instance();
    settings.load();
    settings.setValue(QStringLiteral("Replacement"), QStringLiteral("must-not-save"));
    settings.save();

    expect(!dbHas(QStringLiteral("Replacement")),
           "unusable XML artifacts leave the session read-only (no save)");
    expect(!dbHas(QStringLiteral("AutoConnect")),
           "unusable XML artifacts never seed a default profile over them");
    expect(readFile(xmlPath()) == corruptLive
               && readFile(xmlPath() + QStringLiteral(".bak")) == corruptBackup,
           "unusable XML artifacts are left exactly as found");

    // Second launch retries cleanly (the migration marker was never stamped).
    settings.reset();
    settings.load();
    settings.save();
    expect(readFile(xmlPath()) == corruptLive,
           "the retry launch still refuses to damage the artifacts");
}

void testNoReimportAfterMigration()
{
    const QByteArray live = settingsDocument(10, QStringLiteral("original"));
    expect(writeFile(xmlPath(), live), "import fixture is written");

    AppSettings& settings = AppSettings::instance();
    settings.load();
    settings.setValue(QStringLiteral("Key005"), QStringLiteral("changed-in-db"));
    settings.save();

    // Relaunch: the database is authoritative; the XML (still holding the old
    // value) must NOT be re-imported.
    settings.reset();
    settings.load();
    expect(settings.value(QStringLiteral("Key005")).toString()
               == QStringLiteral("changed-in-db"),
           "the database stays authoritative across relaunches");
    expect(readFile(xmlPath()) == live, "the frozen XML remains untouched");
}

void testXmlChangedNoticeShownOnce()
{
    expect(writeFile(xmlPath(), settingsDocument(10, QStringLiteral("original"))),
           "import fixture is written");

    AppSettings& settings = AppSettings::instance();
    settings.load();
    expect(settings.loadNotice().isEmpty(), "a clean import produces no notice");

    // An older binary "ran" and saved into the frozen XML.
    expect(writeFile(xmlPath(), settingsDocument(10, QStringLiteral("downgraded"))),
           "the frozen XML is modified after migration");

    settings.reset();
    settings.load();
    expect(!settings.loadNotice().isEmpty(),
           "modified-after-migration XML surfaces a one-time notice");
    expect(settings.value(QStringLiteral("Key005")).toString()
               == QStringLiteral("original-5"),
           "the downgrade-era XML changes are not silently merged");

    settings.reset();
    settings.load();
    expect(settings.loadNotice().isEmpty(),
           "the modified-XML notice is shown exactly once");
}

void testCredentialExodus()
{
    QByteArray doc;
    {
        QBuffer buffer(&doc);
        buffer.open(QIODevice::WriteOnly);
        QXmlStreamWriter xml(&buffer);
        xml.writeStartDocument();
        xml.writeStartElement(QStringLiteral("Settings"));
        xml.writeTextElement(QStringLiteral("AutoConnect"), QStringLiteral("True"));
        xml.writeTextElement(QStringLiteral("AutomationBridgeToken"),
                             QStringLiteral("SECRET-TOKEN"));
        xml.writeTextElement(QStringLiteral("MqttPass"), QStringLiteral("hunter2"));
        xml.writeTextElement(
            QStringLiteral("CopyAssist"),
            QStringLiteral("{\"AsrLanguage\":\"en\","
                           "\"AsrRemoteApiKey\":\"sk-SECRET\","
                           "\"AsrRemoteUrl\":\"https://api.example\"}"));
        xml.writeEndElement();
        xml.writeEndDocument();
    }
    expect(writeFile(xmlPath(), doc), "credential fixture is written");

    AppSettings& settings = AppSettings::instance();
    settings.load();

    expect(!dbHas(QStringLiteral("AutomationBridgeToken"))
               && !dbHas(QStringLiteral("MqttPass")),
           "credential rows never reach the database");
    QString copyAssist;
    expect(dbRow(QStringLiteral("CopyAssist"), copyAssist)
               && !copyAssist.contains(QStringLiteral("sk-SECRET"))
               && copyAssist.contains(QStringLiteral("api.example")),
           "the credential FIELD is stripped from the imported document, the "
           "rest of the document survives");
    // The diverted values are available to their features this session.
    expect(settings.takeSessionCredential(QStringLiteral("mqtt_password"))
               == QStringLiteral("hunter2"),
           "a diverted credential is available from the session vault");
    expect(readFile(xmlPath()) == doc,
           "the exodus never modifies the frozen XML (it keeps the user's "
           "only remaining plaintext copy)");

    // The seam guard (PR #4612 review): a credential key can never be written
    // into the store post-migration either — setValue diverts to the vault.
    settings.setValue(QStringLiteral("MqttPass"), QStringLiteral("again"));
    settings.save();
    expect(!dbHas(QStringLiteral("MqttPass")),
           "setValue on a credential key never reaches the database");
    expect(settings.takeSessionCredential(QStringLiteral("mqtt_password"))
               == QStringLiteral("again"),
           "the diverted setValue lands in the session vault instead");
    // A document that re-grows its credential field is stripped at the seam.
    settings.setValue(
        QStringLiteral("CopyAssist"),
        QStringLiteral("{\"AsrLanguage\":\"en\",\"AsrRemoteApiKey\":\"sk-AGAIN\"}"));
    settings.save();
    QString doc2;
    expect(dbRow(QStringLiteral("CopyAssist"), doc2)
               && !doc2.contains(QStringLiteral("sk-AGAIN")),
           "a credential field re-added to a document is stripped at the seam");
    expect(SettingsSanitizer::isSecretKey(QStringLiteral("MqttPass")),
           "the sanitizer redacts the exact exodus names (MqttPass drift hole)");
}

void testCorruptDbRestoresFromBackup()
{
    // Establish a migrated store with a post-migration backup.
    expect(writeFile(xmlPath(), settingsDocument(10, QStringLiteral("original"))),
           "import fixture is written");
    AppSettings& settings = AppSettings::instance();
    settings.load();
    settings.setValue(QStringLiteral("Survivor"), QStringLiteral("via-backup"));
    settings.save();
    // Refresh the backup so it contains Survivor, then corrupt the live DB.
    settings.reset();
    // (reset closed the connection; overwrite the DB with garbage)
    expect(writeFile(dbPath(), QByteArray("this is not a database")),
           "the live database is corrupted");
    // Remove the stale post-migration backup and plant a fresh verified one?
    // No — the post-migration backup is the restoration source; Survivor was
    // added after it was taken, so the restore rolls back to import-day state.
    settings.load();

    expect(!settings.loadNotice().isEmpty(),
           "restoring from a backup surfaces a user-visible notice");
    expect(settings.value(QStringLiteral("Key005")).toString()
               == QStringLiteral("original-5"),
           "the restored backup carries the import-day profile");
    const QDir quarantine(SettingsPaths::quarantineDir());
    expect(!quarantine.entryList(QDir::Files).isEmpty(),
           "the corrupt database is preserved in quarantine");
}

void testCorruptDbReimportsFrozenXml()
{
    const QByteArray live = settingsDocument(10, QStringLiteral("original"));
    expect(writeFile(xmlPath(), live), "import fixture is written");
    AppSettings& settings = AppSettings::instance();
    settings.load();
    settings.reset();

    // Corrupt the DB and remove every backup: the frozen XML is the last line.
    expect(writeFile(dbPath(), QByteArray("garbage")), "database corrupted");
    QDir backups(SettingsPaths::backupsDir());
    const QStringList backupFiles = backups.entryList(QDir::Files);
    for (const QString& name : backupFiles) {
        backups.remove(name);
    }

    settings.load();
    expect(settings.value(QStringLiteral("Key005")).toString()
               == QStringLiteral("original-5"),
           "with no usable backup the frozen XML is re-imported");
    QString value;
    expect(dbRow(QStringLiteral("Key005"), value)
               && value == QStringLiteral("original-5"),
           "the re-import produces a fresh usable database");
}

// PR #4612 review (nigelfenton, demonstrated on Linux): a database held by
// another process's EXCLUSIVE transaction must fail the session, NOT be
// quarantined — POSIX rename() succeeds on open files, so quarantining a
// healthy busy store would strand the other instance's committed writes.
void testLockedDbFailsClosed()
{
    AppSettings& settings = AppSettings::instance();
    settings.load();
    settings.setValue(QStringLiteral("Survivor"), QStringLiteral("intact"));
    settings.save();
    settings.reset();

    SettingsDatabase holder;
    expect(holder.open(dbPath()), "a second connection opens the store");
    expect(holder.beginExclusive(), "the second connection holds EXCLUSIVE");

    settings.load();   // probe hits SQLITE_BUSY after the timeout
    settings.setValue(QStringLiteral("MustNotSave"), QStringLiteral("x"));
    settings.save();
    holder.rollback();
    holder.close();

    expect(QFile::exists(dbPath()),
           "a busy database is never quarantined");
    const QDir quarantine(SettingsPaths::quarantineDir());
    expect(quarantine.entryList(QDir::Files).isEmpty(),
           "no quarantine entry is created for a busy store");
    QString value;
    expect(dbRow(QStringLiteral("Survivor"), value)
               && value == QStringLiteral("intact"),
           "the held store's data is untouched");
    expect(!dbHas(QStringLiteral("MustNotSave")),
           "the locked-out session cannot save");

    settings.reset();
    settings.load();
    expect(settings.value(QStringLiteral("Survivor")).toString()
               == QStringLiteral("intact"),
           "the next launch recovers normally once the lock is gone");
}

void testNewerSchemaOpensReadOnly()
{
    // Create a healthy store, then stamp a schema version from the future.
    AppSettings& settings = AppSettings::instance();
    settings.load();
    settings.setValue(QStringLiteral("FromTheFuture"), QStringLiteral("yes"));
    settings.save();
    settings.reset();

    // PRAGMA user_version lives at byte offset 60 (big-endian) of the SQLite
    // header — patch it directly so no production API can "migrate" it.
    {
        QFile file(dbPath());
        expect(file.open(QIODevice::ReadWrite), "database header is patchable");
        file.seek(60);
        const char future[4] = {0, 0, 0, 99};
        file.write(future, 4);
    }

    settings.load();
    expect(settings.value(QStringLiteral("FromTheFuture")).toString()
               == QStringLiteral("yes"),
           "a newer-schema database is still readable");
    expect(!settings.loadNotice().isEmpty(),
           "a newer-schema database surfaces a read-only notice");

    settings.setValue(QStringLiteral("FromTheFuture"), QStringLiteral("overwritten"));
    settings.save();
    QString value;
    expect(dbRow(QStringLiteral("FromTheFuture"), value)
               && value == QStringLiteral("yes"),
           "a newer-schema database is never written by an older binary");
    expect(!settings.storeWritable(),
           "storeWritable() reports the read-only session, so the Settings "
           "Browser disables its edit affordances");
}

void testDirtyRowSaves()
{
    AppSettings& settings = AppSettings::instance();
    settings.load();

    settings.setValue(QStringLiteral("KeyA"), QStringLiteral("a1"));
    settings.setValue(QStringLiteral("KeyB"), QStringLiteral("b1"));
    settings.save();
    settings.remove(QStringLiteral("KeyA"));
    settings.save();

    QString value;
    expect(!dbHas(QStringLiteral("KeyA")), "a removed key's row is deleted");
    expect(dbRow(QStringLiteral("KeyB"), value) && value == QStringLiteral("b1"),
           "an untouched sibling row survives the removal save");

    // Station rows follow a station rename.
    settings.setStationValue(QStringLiteral("MeterSel"), QStringLiteral("S-Meter"));
    settings.save();
    settings.setStationName(QStringLiteral("Shack2"));
    settings.save();
    settings.reset();
    settings.load();
    expect(settings.stationName() == QStringLiteral("Shack2"),
           "a station rename persists");
    expect(settings.stationValue(QStringLiteral("MeterSel")).toString()
               == QStringLiteral("S-Meter"),
           "station values follow the rename to the new station rows");
}

void testThreeDSliceDepthDefaultAndOptIn()
{
    AppSettings& settings = AppSettings::instance();
    settings.load();

    expect(!DisplaySettings::threeDSliceDepth(),
           "3D Slice Shadow defaults OFF when the Display field is absent, so "
           "an upgrade never changes an existing user's display");

    DisplaySettings::setThreeDSliceDepth(true);
    expect(DisplaySettings::threeDSliceDepth(),
           "a persisted True keeps the user's 3D Slice Shadow opt-in");

    DisplaySettings::setThreeDSliceDepth(false);
    expect(!DisplaySettings::threeDSliceDepth(),
           "3D Slice Shadow can be disabled again after opting in");
}

void testPanMenuStateDefaultAndPerSlotPersistence()
{
    AppSettings& settings = AppSettings::instance();
    settings.load();

    expect(DisplaySettings::panMenuExpanded(0),
           "the panadapter menu defaults expanded when no state exists");
    expect(DisplaySettings::panMenuExpanded(1),
           "each additional pan slot defaults expanded independently");

    DisplaySettings::setPanMenuExpanded(0, false);
    DisplaySettings::setPanMenuExpanded(1, true);
    expect(!DisplaySettings::panMenuExpanded(0),
           "slot zero remembers a collapsed menu");
    expect(DisplaySettings::panMenuExpanded(1),
           "slot one remains expanded when slot zero is collapsed");

    QString persisted;
    expect(settings.readAppRowFromDisk(QStringLiteral("Display"), persisted),
           "pan menu state is committed to the settings database");
    const QJsonObject slotStates = QJsonDocument::fromJson(persisted.toUtf8())
                                      .object()
                                      .value(QStringLiteral("panMenuExpanded"))
                                      .toObject();
    expect(slotStates.value(QStringLiteral("0")).toString() == QStringLiteral("False")
               && slotStates.value(QStringLiteral("1")).toString() == QStringLiteral("True"),
           "all pan slots share one nested Display configuration object");

    settings.reset();
    settings.load();
    expect(!DisplaySettings::panMenuExpanded(0)
               && DisplaySettings::panMenuExpanded(1),
           "independent pan menu states survive a settings reload");
}

// A nickname is stored under a key derived from the radio's MAC, folded to the
// XML-era sanitized convention by Hl2Discovery::nicknameSettingsKey(). The
// SQLite store no longer rejects exotic characters, but the sanitized key IS
// the stored identity for existing users — the round-trip must keep working
// through the real helpers, and it must reach the persisted file, not just the
// in-memory map (the original XML-era bug this scenario was born from).
void testNicknameKeySurvivesDisk()
{
    const QString family = QStringLiteral("hl2");
    const QString serial = QStringLiteral("AA:BB:CC:DD:EE:FF");

    AppSettings& settings = AppSettings::instance();
    settings.load();

    // New-world storage: the (family, MAC, Identity) feature document, where
    // the raw MAC is a legal identity (RFC #4603 PR 3).
    hl2::Hl2Discovery::setNickname(family, serial, QStringLiteral("Pat's Hermes"));
    expect(settings.radioFeature(family, serial, QStringLiteral("Identity"))
                   .value(QStringLiteral("nickname"))
                   .toString()
               == QStringLiteral("Pat's Hermes"),
           "the nickname is stored in the scoped Identity document");

    // Re-read from disk the way a fresh launch does: the name must come back.
    settings.reset();
    settings.load();
    expect(hl2::Hl2Discovery::effectiveNickname(family, serial,
                                                QStringLiteral("Hermes-Lite 2"))
               == QStringLiteral("Pat's Hermes"),
           "a nickname set in a previous session survives restart");

    // Legacy migration: a pre-#4603 sanitized flat key is adopted into the
    // document on first read and the flat key is removed.
    const QString legacySerial = QStringLiteral("AA:BB:CC:DD:EE:00");
    const QString legacyKey = hl2::Hl2Discovery::nicknameSettingsKey(legacySerial);
    settings.setValue(legacyKey, QStringLiteral("Legacy Hermes"));
    settings.save();
    expect(hl2::Hl2Discovery::effectiveNickname(family, legacySerial, QString())
               == QStringLiteral("Legacy Hermes"),
           "a legacy flat-key nickname is still honored");
    expect(!settings.contains(legacyKey),
           "the legacy flat key is removed after the one-shot migration");
    expect(settings.radioFeature(family, legacySerial, QStringLiteral("Identity"))
                   .value(QStringLiteral("nickname"))
                   .toString()
               == QStringLiteral("Legacy Hermes"),
           "the migrated nickname lives in the scoped document");

    expect(hl2::Hl2Discovery::effectiveNickname(family, serial, QString())
               != hl2::Hl2Discovery::effectiveNickname(family, legacySerial,
                                                       QString()),
           "two different MACs keep two different nicknames");

    // Clearing removes both homes.
    hl2::Hl2Discovery::setNickname(family, serial, QString());
    expect(hl2::Hl2Discovery::effectiveNickname(family, serial, QString())
               .isEmpty(),
           "a cleared nickname stays cleared");
}

// The AppSettings surface the Settings Browser rides on (RFC #4603 PR 5):
// station-row deletion, the structured radio_settings enumeration, and the
// writability probe. Exercised through reset()+load() round-trips so every
// claim is about the persisted file, not the in-memory cache.
void testBrowserApi()
{
    AppSettings& settings = AppSettings::instance();
    settings.load();
    expect(settings.storeWritable(),
           "a freshly loaded healthy store reports writable");

    // removeStationValue: the deletion must reach the station_settings rows.
    settings.setStationValue(QStringLiteral("BrowserKeep"), QStringLiteral("k"));
    settings.setStationValue(QStringLiteral("BrowserDrop"), QStringLiteral("d"));
    settings.save();
    settings.removeStationValue(QStringLiteral("BrowserDrop"));
    settings.save();
    settings.reset();
    settings.load();
    expect(settings.stationValue(QStringLiteral("BrowserKeep")).toString()
               == QStringLiteral("k"),
           "an untouched station sibling survives the removal save");
    expect(settings.stationValue(QStringLiteral("BrowserDrop"),
                                 QStringLiteral("<absent>"))
                   .toString()
               == QStringLiteral("<absent>"),
           "a removed station key is deleted from the persisted rows");

    // Structured enumeration: coordinates come back as data, and the
    // family-wide row keeps its "" radioId (the display layer, not the API,
    // owns the "(family-wide)" label).
    settings.setRadioFeature(QStringLiteral("hl2"),
                             QStringLiteral("AA:BB:CC:DD:EE:22"),
                             QStringLiteral("BrowserProbe"), 3,
                             QJsonObject{{QStringLiteral("x"), 1}});
    settings.setRadioFeature(QStringLiteral("hl2"), QString(),
                             QStringLiteral("BrowserProbe"), 3,
                             QJsonObject{{QStringLiteral("x"), 2}});
    bool exactSeen = false;
    bool familySeen = false;
    const auto entries = settings.radioFeatureEntriesForDiagnostics();
    for (const auto& entry : entries) {
        if (entry.family != QStringLiteral("hl2")
            || entry.feature != QStringLiteral("BrowserProbe")) {
            continue;
        }
        if (entry.radioId == QStringLiteral("AA:BB:CC:DD:EE:22")) {
            exactSeen = entry.schemaVersion == 3
                        && entry.value.contains(QStringLiteral("\"x\""));
        } else if (entry.radioId.isEmpty()) {
            familySeen = true;
        }
    }
    expect(exactSeen,
           "the structured enumeration returns the per-radio row with its "
           "schema version and document");
    expect(familySeen,
           "the family-wide default row enumerates with an empty radioId");

    // The formatted diagnostics view is derived from the same enumeration.
    bool formattedSeen = false;
    const auto formatted = settings.radioFeaturesForDiagnostics();
    for (const auto& pair : formatted) {
        if (pair.first == QStringLiteral("hl2/AA:BB:CC:DD:EE:22/BrowserProbe@v3")) {
            formattedSeen = true;
        }
    }
    expect(formattedSeen,
           "the display-string diagnostics stay in lockstep with the "
           "structured enumeration");

    // The Settings Browser's masking parity (PR #4631 review): the SAME
    // predicate that drives recursive display redaction must drive
    // editability, or an operator edits a "[REDACTED]" buffer and saves it
    // over the live secret. Pin the predicate on nigelfenton's bench probe
    // shape — a credential field nested inside an innocuously-named doc.
    const QJsonObject probeDoc{
        {QStringLiteral("enabled"), true},
        {QStringLiteral("nested"),
         QJsonObject{{QStringLiteral("AsrRemoteApiKey"),
                      QStringLiteral("sk-live-SUPERSECRET")}}}};
    expect(SettingsSanitizer::containsSecretField(probeDoc),
           "containsSecretField finds a credential field at depth");
    expect(!SettingsSanitizer::containsSecretField(
               QJsonObject{{QStringLiteral("frequencyHz"), 7100000},
                           {QStringLiteral("mode"), QStringLiteral("CW")}}),
           "containsSecretField stays quiet on a clean operating-state doc");
    // And the two stay in agreement: whatever the display path would mask,
    // the editability predicate must flag (break one on purpose and this
    // pair diverges).
    const QString probeJson = QString::fromUtf8(
        QJsonDocument(probeDoc).toJson(QJsonDocument::Compact));
    const QString redacted =
        SettingsSanitizer::redactedValue(QStringLiteral("CopyAssist"), probeJson);
    expect(redacted.contains(QStringLiteral("[REDACTED]"))
               && !redacted.contains(QStringLiteral("SUPERSECRET")),
           "display redaction masks the nested field the predicate flagged");

    // Cache-vs-disk verification (PR #4631 review, NF0T): a failed save()
    // deliberately KEEPS the change in memory for retry, so a cache re-read
    // is not evidence of persistence. The browser's verification therefore
    // reads the database file. Falsifiable by construction — the control
    // below proves the cache would have lied.
    settings.setValue(QStringLiteral("DiskProbe"), QStringLiteral("committed"));
    settings.save();
    QString disk;
    expect(settings.readAppRowFromDisk(QStringLiteral("DiskProbe"), disk)
               && disk == QStringLiteral("committed"),
           "a committed app row reads back from the database file");
    expect(!settings.readAppRowFromDisk(QStringLiteral("NeverWritten"), disk),
           "a key that was never written is absent from the file");

    // The control: mutate the cache WITHOUT saving. The cache now reports
    // the new value (what the old verification read) while the file still
    // holds the old one (what the browser now reads).
    settings.setValue(QStringLiteral("DiskProbe"), QStringLiteral("uncommitted"));
    expect(settings.value(QStringLiteral("DiskProbe")).toString()
               == QStringLiteral("uncommitted"),
           "control: the cache reports an unsaved change as current");
    expect(settings.readAppRowFromDisk(QStringLiteral("DiskProbe"), disk)
               && disk == QStringLiteral("committed"),
           "the file still holds the last COMMITTED value — a cache read "
           "would have reported an unwritten change as saved");

    settings.setStationValue(QStringLiteral("DiskProbeStation"),
                             QStringLiteral("s1"));
    settings.save();
    expect(settings.readStationRowFromDisk(QStringLiteral("DiskProbeStation"),
                                           disk)
               && disk == QStringLiteral("s1"),
           "a committed station row reads back from the database file");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-app-settings-safety-test"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "[FAIL] could not create temporary settings profile\n");
        return 1;
    }

    QCoreApplication app(argc, argv);
    if (argc != 2) {
        std::fprintf(stderr, "usage: app_settings_safety_test <scenario>\n");
        return 2;
    }

    const QString scenario = QString::fromLocal8Bit(argv[1]);
    if (scenario == QStringLiteral("save-before-load")) {
        testSaveBeforeLoad();
    } else if (scenario == QStringLiteral("xml-import-parity")) {
        testXmlImportParity();
    } else if (scenario == QStringLiteral("first-run")) {
        testFirstRunInitialization();
    } else if (scenario == QStringLiteral("database-file-permissions")) {
        testDatabaseFilePermissions();
    } else if (scenario == QStringLiteral("xml-import-tmp-promotion")) {
        testImportPromotesValidTemp();
    } else if (scenario == QStringLiteral("xml-import-bak-fallback")) {
        testImportFallsBackToBackup();
    } else if (scenario == QStringLiteral("xml-artifacts-unusable")) {
        testUnusableArtifactsFailClosed();
    } else if (scenario == QStringLiteral("no-reimport")) {
        testNoReimportAfterMigration();
    } else if (scenario == QStringLiteral("xml-changed-notice")) {
        testXmlChangedNoticeShownOnce();
    } else if (scenario == QStringLiteral("credential-exodus")) {
        testCredentialExodus();
    } else if (scenario == QStringLiteral("corrupt-db-restore-backup")) {
        testCorruptDbRestoresFromBackup();
    } else if (scenario == QStringLiteral("corrupt-db-reimport-xml")) {
        testCorruptDbReimportsFrozenXml();
    } else if (scenario == QStringLiteral("locked-db-fails-closed")) {
        testLockedDbFailsClosed();
    } else if (scenario == QStringLiteral("newer-schema-readonly")) {
        testNewerSchemaOpensReadOnly();
    } else if (scenario == QStringLiteral("dirty-row-save")) {
        testDirtyRowSaves();
    } else if (scenario == QStringLiteral("display-slice-depth-default")) {
        testThreeDSliceDepthDefaultAndOptIn();
    } else if (scenario == QStringLiteral("display-pan-menu-state")) {
        testPanMenuStateDefaultAndPerSlotPersistence();
    } else if (scenario == QStringLiteral("nickname-key-roundtrip")) {
        testNicknameKeySurvivesDisk();
    } else if (scenario == QStringLiteral("browser-api")) {
        testBrowserApi();
    } else {
        std::fprintf(stderr, "unknown scenario: %s\n", argv[1]);
        return 2;
    }

    return g_failures == 0 ? 0 : 1;
}
