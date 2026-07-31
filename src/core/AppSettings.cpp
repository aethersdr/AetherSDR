#include "AppSettings.h"
#include "GuiClientIdentityPolicy.h"
#include "SettingsCredentialPolicy.h"
#include "SettingsDatabase.h"
#include "SettingsPaths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <QUuid>
#include <QXmlStreamReader>

#ifdef HAVE_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

namespace AetherSDR {

AppSettings& AppSettings::instance()
{
    static AppSettings s;
    return s;
}

AppSettings::~AppSettings() = default;

namespace {

QString validUuidOrEmpty(const QString& value)
{
    const QUuid uuid(value.trimmed());
    return uuid.isNull() ? QString() : uuid.toString(QUuid::WithoutBraces).toUpper();
}

QString machineBinding()
{
    QByteArray unique = QSysInfo::machineUniqueId();
    if (unique.isEmpty()) {
        unique = QSysInfo::machineHostName().toUtf8() + '|'
                 + QSysInfo::kernelType().toUtf8() + '|'
                 + QSysInfo::currentCpuArchitecture().toUtf8();
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(unique, QCryptographicHash::Sha256).toHex());
}

constexpr char kIdentityConfigKey[] = "GuiClientIdentity";

// Credential exodus + seam guard (RFC #4603 proposal E): the known credential
// names live in SettingsCredentialPolicy.h — ONE list shared with the
// sanitizer and the --config CLI so the three cannot drift (PR #4612 review).

QString fileSha256(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool readSettingsFile(const QString& path, QMap<QString, QString>& settings,
                      QMap<QString, QString>& stationSettings,
                      QString& stationName, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error = file.errorString();
        return false;
    }

    QMap<QString, QString> parsedSettings;
    QMap<QString, QString> parsedStationSettings;
    QString parsedStationName = QStringLiteral("AetherSDR");
    QString currentStation;
    bool sawRoot = false;
    QXmlStreamReader xml(&file);

    while (!xml.atEnd()) {
        xml.readNext();

        if (xml.isStartElement()) {
            const QString tag = xml.name().toString();
            if (!sawRoot) {
                if (tag != QStringLiteral("Settings")) {
                    xml.raiseError(QStringLiteral("root element is not Settings"));
                } else {
                    sawRoot = true;
                }
                continue;
            }

            if (tag == parsedStationName && currentStation.isEmpty()) {
                currentStation = tag;
                continue;
            }

            const QString text = xml.readElementText();
            if (!currentStation.isEmpty()) {
                parsedStationSettings.insert(tag, text);
            } else {
                parsedSettings.insert(tag, text);
                if (tag == QStringLiteral("StationName")) {
                    parsedStationName = text;
                }
            }
        } else if (xml.isEndElement()
                   && xml.name().toString() == currentStation) {
            currentStation.clear();
        }
    }

    if (xml.hasError() || !sawRoot) {
        error = xml.hasError() ? xml.errorString()
                               : QStringLiteral("missing Settings root element");
        return false;
    }

    settings = parsedSettings;
    stationSettings = parsedStationSettings;
    stationName = parsedStationName;
    error.clear();
    return true;
}

} // namespace

AppSettings::AppSettings()
{
    const QString configDir = SettingsPaths::configDir();
    QDir().mkpath(configDir);
    m_filePath = SettingsPaths::databasePath();
    m_db = std::make_unique<SettingsDatabase>();

    migrateSettingsPath();
}

QString AppSettings::legacyXmlPath() const
{
    return SettingsPaths::legacyXmlPath();
}

// ─── Path migration (legacy XML) ──────────────────────────────────────────────

void AppSettings::migrateSettingsPath()
{
    const QString xmlPath = SettingsPaths::legacyXmlPath();
    if (QFile::exists(xmlPath)) {
        return;  // already at the correct location
    }

#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    // On Windows and macOS, Qt 6's ConfigLocation resolves to AppConfigLocation
    // (%LOCALAPPDATA%/AetherSDR/AetherSDR on Windows,
    //  ~/Library/Preferences/AetherSDR/AetherSDR on macOS), so appending
    // "/AetherSDR" produced a triple-nested path. Move the file if found there
    // so the XML import below can find it.
    const QString oldPath =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
        + "/AetherSDR/AetherSDR.settings";
    if (QFile::exists(oldPath)) {
        if (QFile::rename(oldPath, xmlPath)) {
            qDebug() << "AppSettings: migrated settings from" << oldPath << "to" << xmlPath;
        } else {
            qWarning() << "AppSettings: failed to migrate from" << oldPath << "to" << xmlPath;
        }
    }
#endif
}

// ─── Load ─────────────────────────────────────────────────────────────────────

void AppSettings::load()
{
    QMutexLocker ioLocker(&m_saveMutex);
    {
        QWriteLocker locker(&m_lock);
        m_loadState = LoadState::Failed;
        m_settings.clear();
        m_stationSettings.clear();
        m_dirtyKeys.clear();
        m_dirtyStationKeys.clear();
        m_stationName = QStringLiteral("AetherSDR");
        m_stationRenamedFrom.clear();
        m_loadNotice.clear();
    }

    // ── Open the database, with corruption quarantine + backup restore ──────
    bool opened = m_db->open(m_filePath);
    if (opened && !m_db->isNewerSchema() && !m_db->quickCheck()
        && !m_db->integrityCheck()) {
        qWarning() << "AppSettings: settings database failed integrity check";
        opened = false;
    }
    if (!opened && m_db->lastOpenWasBusy()) {
        // A busy store is HEALTHY — another process (an import mid-EXCLUSIVE,
        // a second instance) holds it. Never quarantine it: on POSIX,
        // rename() succeeds on open files, so moving it aside would strand
        // the other instance's committed writes in the quarantined inode
        // (PR #4612 review — demonstrated, not theoretical). Fail this
        // session; the next launch retries.
        qWarning() << "AppSettings: settings database is locked by another"
                      " process — read-only session, retry next launch";
        return;
    }
    if (!opened && QFile::exists(m_filePath)) {
        // Confirmed unusable (unreadable or failed integrity). Quarantine and
        // restore are serialized under a lock file so two instances cannot
        // both decide to "recover" — the second one fails its session instead.
        QLockFile recoveryLock(m_filePath + QStringLiteral(".recovery.lock"));
        if (!recoveryLock.tryLock(0)) {
            qWarning() << "AppSettings: another instance is recovering the"
                          " settings store — read-only session";
            return;
        }
        quarantineCorruptStore();
        const bool restored = restoreNewestVerifiedBackup();
        opened = m_db->open(m_filePath);
        if (opened && restored && !m_db->quickCheck()) {
            opened = false;
        }
        if (!opened) {
            qWarning() << "AppSettings: could not establish a usable settings database";
            return;  // Failed — never overwrite what's left on disk
        }
    } else if (!opened) {
        // No file and still cannot create one (permissions, disk full).
        qWarning() << "AppSettings: cannot create settings database at" << m_filePath;
        // Fall back to a read-only session from the legacy XML if available.
        QMap<QString, QString> settings;
        QMap<QString, QString> stationSettings;
        QString stationName;
        QString error;
        if (readSettingsFile(SettingsPaths::legacyXmlPath(), settings,
                             stationSettings, stationName, error)) {
            extractCredentials(settings);
            QWriteLocker locker(&m_lock);
            m_settings = settings;
            m_stationSettings = stationSettings;
            m_stationName = stationName;
            m_loadNotice = QStringLiteral(
                "Settings are running read-only from the previous settings file; "
                "the settings database could not be created.");
        }
        return;  // Failed state — saves refused, retry next launch
    }

    if (m_db->isNewerSchema()) {
        // A newer binary's database: read it, never write it.
        QMap<QString, QString> settings;
        if (!m_db->loadAppSettings(settings)) {
            return;
        }
        QString stationName = settings.value(QStringLiteral("StationName"));
        if (stationName.isEmpty()) {
            stationName = QStringLiteral("AetherSDR");
        }
        QMap<QString, QString> stationSettings;
        m_db->loadStationSettings(stationName, stationSettings);
        QWriteLocker locker(&m_lock);
        m_settings = settings;
        m_stationSettings = stationSettings;
        m_stationName = stationName;
        m_loadState = LoadState::ReadOnly;
        m_loadNotice = QStringLiteral(
            "The settings database was created by a newer AetherSDR version; "
            "settings are read-only in this session and changes will not be saved.");
        qWarning() << "AppSettings: newer-schema database — read-only session";
        return;
    }

    // ── Already migrated (or fresh install stamped): normal load ────────────
    if (!m_db->metaValue(QStringLiteral("migrated_from_xml")).isEmpty()
        || !m_db->metaValue(QStringLiteral("fresh_install")).isEmpty()) {
        if (!loadFromDatabase()) {
            return;
        }

        // Snapshot-only downgrade contract: if an older binary modified the
        // frozen XML after migration, tell the user once that those changes
        // were not carried forward (RFC #4603 proposal C).
        const QString xmlPath = SettingsPaths::legacyXmlPath();
        const QString storedHash =
            m_db->metaValue(QStringLiteral("source_sha256"));
        if (!storedHash.isEmpty() && QFile::exists(xmlPath)
            && m_db->metaValue(QStringLiteral("xml_changed_notice_shown")).isEmpty()) {
            const QString currentHash = fileSha256(xmlPath);
            if (!currentHash.isEmpty() && currentHash != storedHash) {
                QWriteLocker locker(&m_lock);
                m_loadNotice = QStringLiteral(
                    "Settings changed by an older AetherSDR version were not "
                    "carried forward (the database is authoritative after the "
                    "upgrade). The older settings file is preserved at: %1")
                                   .arg(xmlPath);
                m_db->setMetaValue(QStringLiteral("xml_changed_notice_shown"),
                                   QStringLiteral("1"));
            }
        }

        // Bounded rolling automatic backup, at most one per week.
        const QDateTime lastBackup = QDateTime::fromString(
            m_db->metaValue(QStringLiteral("last_auto_backup")), Qt::ISODate);
        if (!lastBackup.isValid()
            || lastBackup.daysTo(QDateTime::currentDateTimeUtc()) >= 7) {
            if (!writeRollingBackup(QStringLiteral("auto")).isEmpty()) {
                m_db->setMetaValue(
                    QStringLiteral("last_auto_backup"),
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
                pruneBackups(QStringLiteral("auto"), 5);
            }
        }
        return;
    }

    // ── Unmigrated database: import the legacy XML store ────────────────────
    const QString xmlPath = SettingsPaths::legacyXmlPath();
    const QString tmpPath = xmlPath + QStringLiteral(".tmp");
    const QString bakPath = xmlPath + QStringLiteral(".bak");

    QMap<QString, QString> settings;
    QMap<QString, QString> stationSettings;
    QString stationName;
    QString error;
    QString sourcePath;

    if (QFile::exists(xmlPath)
        && readSettingsFile(xmlPath, settings, stationSettings, stationName,
                            error)) {
        sourcePath = xmlPath;
    } else {
        if (QFile::exists(xmlPath)) {
            qWarning() << "AppSettings: cannot parse" << xmlPath << ':' << error;
        }
        // The XML-era recovery ladder: a validated .tmp is the newest intended
        // state after an interrupted XML-era save; else fall back to .bak.
        if (QFile::exists(tmpPath)
            && readSettingsFile(tmpPath, settings, stationSettings, stationName,
                                error)) {
            sourcePath = tmpPath;
            qDebug() << "AppSettings: importing from interrupted XML commit";
        } else if (QFile::exists(bakPath)
                   && readSettingsFile(bakPath, settings, stationSettings,
                                       stationName, error)) {
            sourcePath = bakPath;
            qDebug() << "AppSettings: importing from XML backup";
        }
    }

    if (!sourcePath.isEmpty()) {
        if (importLegacyXml(std::move(settings), std::move(stationSettings),
                            stationName, sourcePath)) {
            return;
        }
        // Import failed: run this session read-only from the parsed XML so the
        // app is usable; the import retries on the next launch (no marker).
        qWarning() << "AppSettings: XML import failed — read-only session,"
                      " will retry next launch";
        return;
    }

    // XML-era artifacts prove this is not a true first launch. If none was
    // usable, stay failed so startup code cannot replace them with defaults.
    if (QFile::exists(xmlPath) || QFile::exists(tmpPath) || QFile::exists(bakPath)) {
        qWarning() << "AppSettings: legacy settings exist but none are readable"
                      " — refusing to start fresh over them";
        return;
    }

    // Genuine first launch (or a legacy QSettings-era install).
    {
        QWriteLocker locker(&m_lock);
        m_loadState = LoadState::ReadyToSave;
    }
    // migrateFromQSettings() ends in save(), which takes m_saveMutex itself.
    ioLocker.unlock();
    migrateFromQSettings();
    m_db->setMetaValue(QStringLiteral("fresh_install"), QStringLiteral("1"));
    m_db->setMetaValue(QStringLiteral("created_by_version"),
                       QCoreApplication::applicationVersion());
}

bool AppSettings::loadFromDatabase()
{
    QMap<QString, QString> settings;
    if (!m_db->loadAppSettings(settings)) {
        qWarning() << "AppSettings: failed to read settings rows:"
                   << m_db->lastError();
        return false;
    }
    QString stationName = settings.value(QStringLiteral("StationName"));
    if (stationName.isEmpty()) {
        stationName = QStringLiteral("AetherSDR");
    }
    QMap<QString, QString> stationSettings;
    if (!m_db->loadStationSettings(stationName, stationSettings)) {
        return false;
    }

    QWriteLocker locker(&m_lock);
    m_settings = settings;
    m_stationSettings = stationSettings;
    m_stationName = stationName;
    m_loadState = LoadState::ReadyToSave;
    qDebug() << "AppSettings: loaded" << m_settings.size() << "settings +"
             << m_stationSettings.size() << "station settings from" << m_filePath;
    return true;
}

// ─── XML import ───────────────────────────────────────────────────────────────

void AppSettings::extractCredentials(QMap<QString, QString>& settings)
{
    for (const auto& entry : SettingsCredentialPolicy::kFlatCredentials) {
        const QString key = QString::fromLatin1(entry.settingsKey);
        const auto it = settings.find(key);
        if (it != settings.end()) {
            if (!it.value().isEmpty()) {
                m_sessionCredentials.insert(QString::fromLatin1(entry.keychainKey),
                                            it.value());
            }
            settings.erase(it);
            qDebug() << "AppSettings: diverted credential key" << key
                     << "out of the settings store";
        }
    }
    for (const auto& entry : SettingsCredentialPolicy::kDocFieldCredentials) {
        const QString docKey = QString::fromLatin1(entry.docKey);
        const auto it = settings.find(docKey);
        if (it == settings.end()) {
            continue;
        }
        QJsonObject obj = QJsonDocument::fromJson(it.value().toUtf8()).object();
        const QString field = QString::fromLatin1(entry.field);
        if (!obj.contains(field)) {
            continue;
        }
        const QString secret = obj.value(field).toString();
        if (!secret.isEmpty()
            && !m_sessionCredentials.contains(QString::fromLatin1(entry.keychainKey))) {
            m_sessionCredentials.insert(QString::fromLatin1(entry.keychainKey),
                                        secret);
        }
        obj.remove(field);
        it.value() = QString::fromUtf8(
            QJsonDocument(obj).toJson(QJsonDocument::Compact));
        qDebug() << "AppSettings: diverted credential field" << docKey << '/'
                 << field << "out of the settings store";
    }
}

void AppSettings::persistVaultToKeychain()
{
#ifdef HAVE_KEYCHAIN
    if (QCoreApplication::instance() == nullptr) {
        qWarning() << "AppSettings: no application instance — imported"
                      " credentials stay session-only";
        return;
    }
    for (auto it = m_sessionCredentials.cbegin();
         it != m_sessionCredentials.cend(); ++it) {
        auto* job = new QKeychain::WritePasswordJob(QStringLiteral("AetherSDR"));
        job->setAutoDelete(false);
        job->setKey(it.key());
        job->setTextData(it.value());
        // The import runs during startup, before app.exec() — pump a local
        // loop, bounded, so the write is done before anything reads it back.
        QEventLoop loop;
        QObject::connect(job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        job->start();
        loop.exec();
        if (job->error() != QKeychain::NoError) {
            qWarning() << "AppSettings: keychain write for imported credential"
                       << it.key() << "failed:" << job->errorString()
                       << "— kept session-only";
        }
        delete job;
    }
#else
    if (!m_sessionCredentials.isEmpty()) {
        qWarning() << "AppSettings: built without keychain support —"
                   << m_sessionCredentials.size()
                   << "imported credential(s) stay session-only and must be"
                      " re-entered next launch";
    }
#endif
}

bool AppSettings::importLegacyXml(QMap<QString, QString> settings,
                                  QMap<QString, QString> stationSettings,
                                  const QString& stationName,
                                  const QString& sourcePath)
{
    // Secrets never reach the database (RFC #4603 proposal E).
    extractCredentials(settings);

    // Make the parsed values available for this session regardless of how the
    // import itself ends — on failure the session is read-only (state Failed).
    {
        QWriteLocker locker(&m_lock);
        m_settings = settings;
        m_stationSettings = stationSettings;
        m_stationName = stationName.isEmpty() ? QStringLiteral("AetherSDR")
                                              : stationName;
    }

    if (!m_db->beginExclusive()) {
        persistVaultToKeychain();
        return false;
    }

    // Another instance may have won the race while we parsed — the marker is
    // re-checked inside the exclusive transaction.
    if (!m_db->metaValue(QStringLiteral("migrated_from_xml")).isEmpty()) {
        m_db->rollback();
        qDebug() << "AppSettings: another instance completed the migration";
        persistVaultToKeychain();
        return loadFromDatabase();
    }

    bool ok = true;
    for (auto it = settings.cbegin(); it != settings.cend() && ok; ++it) {
        ok = m_db->upsertApp(it.key(), it.value());
    }
    const QString effectiveStation =
        stationName.isEmpty() ? QStringLiteral("AetherSDR") : stationName;
    for (auto it = stationSettings.cbegin(); it != stationSettings.cend() && ok;
         ++it) {
        ok = m_db->upsertStation(effectiveStation, it.key(), it.value());
    }

    // Verify inside the transaction: row count and per-key readback equality
    // against the parsed source (RFC #4603 proposal C).
    if (ok) {
        ok = m_db->appCount() == settings.size();
        if (!ok) {
            qWarning() << "AppSettings: import row count mismatch";
        }
    }
    if (ok) {
        for (auto it = settings.cbegin(); it != settings.cend(); ++it) {
            QString readBack;
            if (!m_db->readApp(it.key(), readBack) || readBack != it.value()) {
                qWarning() << "AppSettings: import verification failed for key"
                           << it.key();
                ok = false;
                break;
            }
        }
    }
    // Station rows get the same treatment as app rows — symmetric readback
    // verification, not just an unfailed upsert chain (PR #4612 review).
    if (ok && !stationSettings.isEmpty()) {
        QMap<QString, QString> stationReadBack;
        ok = m_db->loadStationSettings(effectiveStation, stationReadBack)
             && stationReadBack == stationSettings;
        if (!ok) {
            qWarning() << "AppSettings: station-settings import verification"
                          " failed";
        }
    }

    if (ok) {
        const QFileInfo sourceInfo(sourcePath);
        ok = m_db->setMetaValue(QStringLiteral("migrated_from_xml"),
                                QStringLiteral("1"))
             && m_db->setMetaValue(QStringLiteral("migrated_at"),
                                   QDateTime::currentDateTimeUtc().toString(Qt::ISODate))
             && m_db->setMetaValue(QStringLiteral("migrated_by_version"),
                                   QCoreApplication::applicationVersion())
             && m_db->setMetaValue(QStringLiteral("migrated_key_count"),
                                   QString::number(settings.size()))
             && m_db->setMetaValue(QStringLiteral("source_path"), sourcePath)
             && m_db->setMetaValue(QStringLiteral("source_mtime"),
                                   sourceInfo.lastModified().toUTC().toString(Qt::ISODate))
             && m_db->setMetaValue(QStringLiteral("source_sha256"),
                                   fileSha256(sourcePath));
    }

    if (!ok) {
        m_db->rollback();
        persistVaultToKeychain();
        return false;
    }
    if (!m_db->commit()) {
        m_db->rollback();
        persistVaultToKeychain();
        return false;
    }

    {
        QWriteLocker locker(&m_lock);
        m_loadState = LoadState::ReadyToSave;
    }
    qDebug() << "AppSettings: imported" << settings.size() << "settings +"
             << stationSettings.size() << "station settings from" << sourcePath
             << "— the file stays in place as a frozen snapshot";

    persistVaultToKeychain();

    // A verified backup immediately after migration (RFC #4603 proposal E).
    if (!writeRollingBackup(QStringLiteral("postmigration")).isEmpty()) {
        m_db->setMetaValue(QStringLiteral("last_auto_backup"),
                           QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    }
    return true;
}

// ─── Corruption handling & backups ────────────────────────────────────────────

void AppSettings::quarantineCorruptStore()
{
    m_db->close();
    const QString dir = SettingsPaths::quarantineDir();
    QDir().mkpath(dir);
    const QString stamp = QDateTime::currentDateTimeUtc().toString(
        QStringLiteral("yyyyMMdd-HHmmss"));
    for (const QString& suffix :
         {QStringLiteral(""), QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        const QString source = m_filePath + suffix;
        if (QFile::exists(source)) {
            const QString dest = dir + QStringLiteral("/AetherSDR-") + stamp
                                 + QStringLiteral(".db") + suffix;
            if (!QFile::rename(source, dest)) {
                qWarning() << "AppSettings: could not quarantine" << source;
            }
        }
    }
    qWarning() << "AppSettings: quarantined corrupt settings database to" << dir;
}

bool AppSettings::restoreNewestVerifiedBackup()
{
    QDir backups(SettingsPaths::backupsDir());
    const QStringList candidates = backups.entryList(
        {QStringLiteral("*.db")}, QDir::Files, QDir::Name | QDir::Reversed);
    for (const QString& name : candidates) {
        const QString path = backups.filePath(name);
        if (!SettingsDatabase::verifyBackupFile(path)) {
            qWarning() << "AppSettings: backup failed verification, skipping"
                       << path;
            continue;
        }
        if (!QFile::copy(path, m_filePath)) {
            qWarning() << "AppSettings: could not copy backup into place:" << path;
            continue;
        }
        QFile::setPermissions(m_filePath,
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        const QDateTime when = QFileInfo(path).lastModified();
        const qint64 days = when.daysTo(QDateTime::currentDateTime());
        {
            QWriteLocker locker(&m_lock);
            m_loadNotice = QStringLiteral(
                "The settings database was corrupt and has been restored from "
                "the backup \"%1\" (%2 day(s) old). The corrupt files were "
                "preserved under: %3")
                               .arg(name)
                               .arg(days)
                               .arg(SettingsPaths::quarantineDir());
        }
        qWarning() << "AppSettings: restored settings from backup" << name;
        return true;
    }
    return false;
}

QString AppSettings::writeRollingBackup(const QString& tag)
{
    if (!m_db->isOpen()) {
        return {};
    }
    const QString dir = SettingsPaths::backupsDir();
    QDir().mkpath(dir);
    const QString stamp = QDateTime::currentDateTimeUtc().toString(
        QStringLiteral("yyyyMMdd-HHmmss"));
    const QString path = dir + QStringLiteral("/AetherSDR-") + stamp
                         + QStringLiteral("-") + tag + QStringLiteral(".db");
    if (!m_db->backupTo(path)) {
        return {};
    }
    if (!SettingsDatabase::verifyBackupFile(path)) {
        qWarning() << "AppSettings: freshly written backup failed verification:"
                   << path;
        QFile::remove(path);
        return {};
    }
    qDebug() << "AppSettings: wrote verified settings backup" << path;
    return path;
}

void AppSettings::pruneBackups(const QString& tag, int keep)
{
    QDir backups(SettingsPaths::backupsDir());
    const QStringList entries = backups.entryList(
        {QStringLiteral("*-") + tag + QStringLiteral(".db")}, QDir::Files,
        QDir::Name | QDir::Reversed);
    for (int i = keep; i < entries.size(); ++i) {
        backups.remove(entries.at(i));
    }
}

// ─── Save ─────────────────────────────────────────────────────────────────────

void AppSettings::save()
{
    if (QCoreApplication* app = QCoreApplication::instance()) {
        if (app->property("AetherSettingsResetInProgress").toBool()) {
            qWarning() << "AppSettings: skipping save during reset-triggered shutdown";
            return;
        }
    }

    QMutexLocker ioLocker(&m_saveMutex);

    // Steal the dirty state and snapshot the values it refers to.
    QSet<QString> dirtyKeys;
    QSet<QString> dirtyStationKeys;
    QMap<QString, QString> keySnapshot;          // dirty key -> value (present)
    QSet<QString> removedKeys;                   // dirty keys now absent
    QMap<QString, QString> stationSnapshot;
    QSet<QString> removedStationKeys;
    QString stationName;
    QString renamedFrom;
    {
        QWriteLocker locker(&m_lock);
        if (m_loadState == LoadState::ReadOnly) {
            qWarning() << "AppSettings: refusing to save — database was created"
                          " by a newer version (read-only session)";
            return;
        }
        if (m_loadState != LoadState::ReadyToSave) {
            qWarning() << "AppSettings: refusing to save before a successful load";
            return;
        }
        if (m_dirtyKeys.isEmpty() && m_dirtyStationKeys.isEmpty()
            && m_stationRenamedFrom.isEmpty()) {
            return;  // nothing to do — keeps the 330 legacy save() calls cheap
        }
        dirtyKeys = std::move(m_dirtyKeys);
        m_dirtyKeys = {};
        dirtyStationKeys = std::move(m_dirtyStationKeys);
        m_dirtyStationKeys = {};
        renamedFrom = m_stationRenamedFrom;
        m_stationRenamedFrom.clear();
        stationName = m_stationName;

        for (const QString& key : dirtyKeys) {
            const auto it = m_settings.constFind(key);
            if (it != m_settings.constEnd()) {
                keySnapshot.insert(key, it.value());
            } else {
                removedKeys.insert(key);
            }
        }
        if (!renamedFrom.isEmpty()) {
            // Station rename: rewrite the whole station section under the new
            // name (mirrors the XML era, where the section was one element).
            stationSnapshot = m_stationSettings;
        } else {
            for (const QString& key : dirtyStationKeys) {
                const auto it = m_stationSettings.constFind(key);
                if (it != m_stationSettings.constEnd()) {
                    stationSnapshot.insert(key, it.value());
                } else {
                    removedStationKeys.insert(key);
                }
            }
        }
    }

    bool ok = m_db->begin();
    if (ok) {
        for (auto it = keySnapshot.cbegin(); it != keySnapshot.cend() && ok; ++it) {
            ok = m_db->upsertApp(it.key(), it.value());
        }
        for (const QString& key : removedKeys) {
            if (!ok) {
                break;
            }
            ok = m_db->removeApp(key);
        }
        if (ok && !renamedFrom.isEmpty()) {
            ok = m_db->removeStationAll(renamedFrom)
                 && m_db->removeStationAll(stationName);
        }
        for (auto it = stationSnapshot.cbegin(); it != stationSnapshot.cend() && ok;
             ++it) {
            ok = m_db->upsertStation(stationName, it.key(), it.value());
        }
        for (const QString& key : removedStationKeys) {
            if (!ok) {
                break;
            }
            ok = m_db->removeStation(stationName, key);
        }
        if (ok) {
            ok = m_db->commit();
        }
        if (!ok) {
            m_db->rollback();
        }
    }

    if (!ok) {
        qWarning() << "AppSettings: save failed:" << m_db->lastError()
                   << "— changes kept in memory for retry";
        QWriteLocker locker(&m_lock);
        m_dirtyKeys.unite(dirtyKeys);
        m_dirtyStationKeys.unite(dirtyStationKeys);
        if (m_stationRenamedFrom.isEmpty()) {
            m_stationRenamedFrom = renamedFrom;
        }
    }
}

void AppSettings::reset()
{
    // Lock ORDER matches save()/the feature-doc writers (m_saveMutex outer,
    // m_lock inner): the close below cannot interleave with a writer that is
    // mid-statement under m_saveMutex (PR #4614 review — the sqlite handle
    // has no API armor, so a closed-underneath call is a crash, not an error).
    QMutexLocker ioLocker(&m_saveMutex);
    QWriteLocker locker(&m_lock);
    m_guiClientLock.reset();
    m_settings.clear();
    m_stationSettings.clear();
    m_dirtyKeys.clear();
    m_dirtyStationKeys.clear();
    m_stationName = "AetherSDR";
    m_stationRenamedFrom.clear();
    m_sessionCredentials.clear();
    m_loadNotice.clear();
    m_loadState = LoadState::NotAttempted;
    m_persistentGuiClientId.clear();
    m_effectiveGuiClientId.clear();
    m_effectiveStationName.clear();
    m_automationIdentity.clear();
    m_automationAgentName.clear();
    m_guiClientIdentityInitialized = false;
    m_guiClientIdentityTransient = false;
    if (m_db) {
        m_db->close();
    }
}

QString AppSettings::shutdownForReset()
{
    QMutexLocker ioLocker(&m_saveMutex);
    QString backupPath;
    if (m_db && m_db->isOpen()) {
        backupPath = writeRollingBackup(QStringLiteral("prereset"));
        pruneBackups(QStringLiteral("prereset"), 3);
        m_db->close();  // checkpoints the WAL and releases the file handles
    }
    return backupPath;
}

QString AppSettings::loadNotice() const
{
    QReadLocker locker(&m_lock);
    return m_loadNotice;
}

QString AppSettings::takeSessionCredential(const QString& keychainKey)
{
    QWriteLocker locker(&m_lock);
    return m_sessionCredentials.take(keychainKey);
}

void AppSettings::setSessionCredential(const QString& keychainKey,
                                       const QString& value)
{
    QWriteLocker locker(&m_lock);
    if (value.isEmpty()) {
        m_sessionCredentials.remove(keychainKey);
    } else {
        m_sessionCredentials.insert(keychainKey, value);
    }
}

// ─── Top-level accessors ──────────────────────────────────────────────────────

QVariant AppSettings::value(const QString& key, const QVariant& defaultValue) const
{
    QReadLocker locker(&m_lock);
    const auto it = m_settings.constFind(key);
    if (it != m_settings.constEnd()) {
        return it.value();
    }
    return defaultValue;
}

void AppSettings::setValue(const QString& key, const QVariant& val)
{
    QString str = val.toString();

    // The credential seam (RFC #4603 proposal E, enforced HERE rather than by
    // caller discipline — PR #4612 review): a known credential key never
    // reaches the store. Divert to the session vault so a legacy caller keeps
    // working this session; its feature owns moving it to the keychain.
    if (SettingsCredentialPolicy::isFlatCredentialKey(key)) {
        qWarning() << "AppSettings: refusing to store credential key" << key
                   << "— diverted to the session vault (keychain is the"
                      " persistent credential store)";
        setSessionCredential(SettingsCredentialPolicy::keychainKeyForFlat(key),
                             str);
        return;
    }
    // A credential FIELD inside a feature document is stripped the same way.
    if (SettingsCredentialPolicy::isCredentialDocKey(key)) {
        for (const auto& entry : SettingsCredentialPolicy::kDocFieldCredentials) {
            if (key != QLatin1String(entry.docKey)) {
                continue;
            }
            QJsonObject obj = QJsonDocument::fromJson(str.toUtf8()).object();
            const QString field = QString::fromLatin1(entry.field);
            if (!obj.contains(field)) {
                continue;
            }
            const QString secret = obj.value(field).toString();
            if (!secret.isEmpty()) {
                setSessionCredential(QString::fromLatin1(entry.keychainKey),
                                     secret);
            }
            obj.remove(field);
            str = QString::fromUtf8(
                QJsonDocument(obj).toJson(QJsonDocument::Compact));
            qWarning() << "AppSettings: stripped credential field" << key << '/'
                       << field << "before storing the document";
        }
    }

    QWriteLocker locker(&m_lock);
    const auto it = m_settings.constFind(key);
    if (it != m_settings.constEnd() && it.value() == str) {
        return;  // unchanged — don't dirty the row
    }
    m_settings.insert(key, str);
    m_dirtyKeys.insert(key);
}

void AppSettings::remove(const QString& key)
{
    QWriteLocker locker(&m_lock);
    if (m_settings.remove(key) > 0) {
        m_dirtyKeys.insert(key);
    }
}

bool AppSettings::contains(const QString& key) const
{
    QReadLocker locker(&m_lock);
    return m_settings.contains(key);
}

QStringList AppSettings::allKeysForDiagnostics() const
{
    QReadLocker locker(&m_lock);
    return m_settings.keys();
}

QStringList AppSettings::stationKeysForDiagnostics() const
{
    QReadLocker locker(&m_lock);
    return m_stationSettings.keys();
}

QList<QPair<QString, QString>> AppSettings::radioFeaturesForDiagnostics() const
{
    QMutexLocker ioLocker(&m_saveMutex);
    QList<QPair<QString, QString>> out;
    if (!m_db || !m_db->isOpen()) {
        return out;
    }
    QList<SettingsDatabase::RadioFeatureRow> rows;
    if (!m_db->listRadioFeatures(rows)) {
        return out;
    }
    for (const auto& row : rows) {
        out.append({QStringLiteral("%1/%2/%3@v%4")
                        .arg(row.family,
                             row.radioId.isEmpty() ? QStringLiteral("<family>")
                                                   : row.radioId,
                             row.feature)
                        .arg(row.schemaVersion),
                    row.value});
    }
    return out;
}

// ─── Radio-scoped feature documents (RFC #4603) ──────────────────────────────

QJsonObject AppSettings::radioFeatureExact(const QString& family,
                                           const QString& radioId,
                                           const QString& feature,
                                           int* schemaVersionOut) const
{
    if (schemaVersionOut != nullptr) {
        *schemaVersionOut = 0;
    }
    QMutexLocker ioLocker(&m_saveMutex);
    if (!m_db || !m_db->isOpen()) {
        return {};
    }
    int version = 0;
    QString value;
    if (!m_db->readRadioFeature(family, radioId, feature, version, value)) {
        return {};
    }
    QJsonParseError parseError{};
    const QJsonDocument parsed =
        QJsonDocument::fromJson(value.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
        qWarning() << "AppSettings: radio feature document is corrupt —"
                   << family << radioId << feature << ':'
                   << parseError.errorString();
        return {};
    }
    if (schemaVersionOut != nullptr) {
        *schemaVersionOut = version;
    }
    return parsed.object();
}

QJsonObject AppSettings::radioFeature(const QString& family,
                                      const QString& radioId,
                                      const QString& feature,
                                      int* schemaVersionOut) const
{
    if (schemaVersionOut != nullptr) {
        *schemaVersionOut = 0;
    }
    QMutexLocker ioLocker(&m_saveMutex);
    if (!m_db || !m_db->isOpen()) {
        return {};
    }

    // Read one row and parse it; a row that exists but does not parse is a
    // CORRUPT document — logged, never silently treated as "nothing stored"
    // (PR #4614 review).
    const auto readRow = [this, &family, &feature](const QString& id,
                                                   int& version,
                                                   QJsonObject& doc) {
        QString value;
        if (!m_db->readRadioFeature(family, id, feature, version, value)) {
            return false;
        }
        QJsonParseError parseError{};
        const QJsonDocument parsed =
            QJsonDocument::fromJson(value.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
            qWarning() << "AppSettings: radio feature document is corrupt —"
                       << family << id << feature << ':'
                       << parseError.errorString();
            return false;
        }
        doc = parsed.object();
        return true;
    };

    int version = 0;
    QJsonObject doc;
    // Exact radio first; a missing OR corrupt exact row falls back to the
    // family-wide default row (radioId "").
    if (readRow(radioId, version, doc)
        || (!radioId.isEmpty() && readRow(QString(), version, doc))) {
        if (schemaVersionOut != nullptr) {
            *schemaVersionOut = version;
        }
        return doc;
    }
    return {};
}

bool AppSettings::setRadioFeature(const QString& family, const QString& radioId,
                                  const QString& feature, int schemaVersion,
                                  const QJsonObject& doc)
{
    if (family.isEmpty() || feature.isEmpty()) {
        qWarning() << "AppSettings: refusing radio feature write without a"
                      " family and feature name";
        return false;
    }
    QMutexLocker ioLocker(&m_saveMutex);
    {
        QReadLocker locker(&m_lock);
        if (m_loadState != LoadState::ReadyToSave) {
            qWarning() << "AppSettings: refusing radio feature write —"
                          " store not writable";
            return false;
        }
    }
    if (!m_db || !m_db->isOpen()) {
        return false;   // reset() may have closed the store (PR #4614 review)
    }
    const QString value = QString::fromUtf8(
        QJsonDocument(doc).toJson(QJsonDocument::Compact));
    if (!m_db->begin()) {
        return false;
    }
    if (!m_db->upsertRadioFeature(family, radioId, feature, schemaVersion, value)
        || !m_db->commit()) {
        m_db->rollback();
        qWarning() << "AppSettings: radio feature write failed:"
                   << m_db->lastError();
        return false;
    }
    return true;
}

bool AppSettings::removeRadioFeature(const QString& family,
                                     const QString& radioId,
                                     const QString& feature)
{
    QMutexLocker ioLocker(&m_saveMutex);
    {
        QReadLocker locker(&m_lock);
        if (m_loadState != LoadState::ReadyToSave) {
            return false;
        }
    }
    if (!m_db || !m_db->isOpen()) {
        return false;   // reset() may have closed the store (PR #4614 review)
    }
    if (!m_db->removeRadioFeature(family, radioId, feature)) {
        qWarning() << "AppSettings: radio feature remove failed:" << family
                   << radioId << feature << "—" << m_db->lastError();
        return false;
    }
    return true;
}

// ─── Per-station accessors ────────────────────────────────────────────────────

QVariant AppSettings::stationValue(const QString& key, const QVariant& defaultValue) const
{
    QReadLocker locker(&m_lock);
    const auto it = m_stationSettings.constFind(key);
    if (it != m_stationSettings.constEnd()) {
        return it.value();
    }
    return defaultValue;
}

void AppSettings::setStationValue(const QString& key, const QVariant& val)
{
    const QString str = val.toString();
    QWriteLocker locker(&m_lock);
    const auto it = m_stationSettings.constFind(key);
    if (it != m_stationSettings.constEnd() && it.value() == str) {
        return;
    }
    m_stationSettings.insert(key, str);
    m_dirtyStationKeys.insert(key);
}

QString AppSettings::stationName() const
{
    QReadLocker locker(&m_lock);
    return m_stationName;
}

void AppSettings::setStationName(const QString& name)
{
    QWriteLocker locker(&m_lock);
    if (name == m_stationName) {
        return;
    }
    if (m_stationRenamedFrom.isEmpty()) {
        m_stationRenamedFrom = m_stationName;
    }
    m_stationName = name;
    m_settings.insert("StationName", name);
    m_dirtyKeys.insert("StationName");
}

void AppSettings::initializeGuiClientIdentity()
{
    if (m_guiClientIdentityInitialized) {
        return;
    }
    m_guiClientIdentityInitialized = true;

    const QJsonObject stored = QJsonDocument::fromJson(
        value(QString::fromLatin1(kIdentityConfigKey)).toString().toUtf8()).object();
    QString persistent = validUuidOrEmpty(
        stored.value(QStringLiteral("PersistentId")).toString());
    if (persistent.isEmpty()) {
        persistent = validUuidOrEmpty(value(QStringLiteral("GUIClientID")).toString());
    }
    if (persistent.isEmpty()) {
        persistent = QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
    }

    const QString currentBinding = machineBinding();
    const QString storedBinding = stored.value(QStringLiteral("MachineBinding")).toString();
    if (!storedBinding.isEmpty() && !currentBinding.isEmpty()
        && storedBinding != currentBinding) {
        persistent = QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
        qWarning() << "AppSettings: settings came from another machine; rotated GUI client ID";
    }

    m_persistentGuiClientId = persistent;
    QJsonObject identityConfig{
        {QStringLiteral("PersistentId"), persistent},
        {QStringLiteral("MachineBinding"), currentBinding},
    };
    const QString encoded = QString::fromUtf8(
        QJsonDocument(identityConfig).toJson(QJsonDocument::Compact));
    // Only persist when something actually changed, so the common case (a
    // stored id read back unchanged) issues no save and a second concurrent
    // instance doesn't rewrite the store. The one racy case — two fresh
    // installs with no stored id launched simultaneously — writes divergent
    // persistent ids last-writer-wins, but each instance's *effective* id
    // stays distinct via the lock below, so runtime behavior is unaffected.
    if (value(QString::fromLatin1(kIdentityConfigKey)).toString() != encoded
        || value(QStringLiteral("GUIClientID")).toString() != persistent) {
        setValue(QString::fromLatin1(kIdentityConfigKey), encoded);
        // Compatibility mirror for older builds and existing support tooling.
        setValue(QStringLiteral("GUIClientID"), persistent);
        save();
    }

    const bool automation = qEnvironmentVariableIsSet("AETHER_AUTOMATION");
    if (automation) {
        m_automationIdentity = qEnvironmentVariable("AETHER_AUTOMATION_IDENTITY").trimmed();
        if (m_automationIdentity.isEmpty()) {
            m_automationIdentity = qEnvironmentVariable("AETHER_AUTOMATION_SOCKET").trimmed();
        }
        if (m_automationIdentity.isEmpty()) {
            m_automationIdentity = qEnvironmentVariable("AETHER_AUTOMATION_LABEL").trimmed();
        }
        if (m_automationIdentity.isEmpty()) {
            m_automationIdentity = QStringLiteral("pid-%1")
                                       .arg(QCoreApplication::applicationPid());
        }
        m_effectiveGuiClientId =
            GuiClientIdentityPolicy::automationClientId(m_automationIdentity);
        m_guiClientIdentityTransient = true;
        m_automationAgentName = GuiClientIdentityPolicy::automationAgentName(
            qEnvironmentVariable("AETHER_AUTOMATION_AGENT_NAME"),
            qEnvironmentVariable("AETHER_AUTOMATION_STATION"),
            qEnvironmentVariable("AETHER_AUTOMATION_LABEL"));
        m_effectiveStationName =
            GuiClientIdentityPolicy::protocolSafeStation(m_automationAgentName);
        return;
    }

    m_guiClientLock = std::make_unique<QLockFile>(m_filePath + QStringLiteral(".gui-client.lock"));
    // Disable time-based staleness on purpose: QLockFile stamps the lock mtime
    // once at creation and never refreshes it while held, so a non-zero stale
    // time would let a second instance declare a *live* long-running instance's
    // lock stale after the interval and steal the persistent ID — reintroducing
    // the exact collision this guards against. QLockFile's same-machine
    // PID-alive check still reclaims a crashed instance's lock; the only
    // residual is a crashed PID later reused by an unrelated process, which
    // leaves this install on a transient ID until the lock file is removed.
    m_guiClientLock->setStaleLockTime(0);
    if (m_guiClientLock->tryLock(0)) {
        m_effectiveGuiClientId = persistent;
    } else {
        m_effectiveGuiClientId =
            QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
        m_guiClientIdentityTransient = true;
        qWarning() << "AppSettings: another local process owns the persistent GUI client ID;"
                      " using a process-scoped ID";
    }

    m_effectiveStationName = GuiClientIdentityPolicy::protocolSafeStation(
        value(QStringLiteral("StationName")).toString());
    if (m_effectiveStationName.isEmpty()) {
        m_effectiveStationName =
            GuiClientIdentityPolicy::protocolSafeStation(QSysInfo::machineHostName());
    }
}

QString AppSettings::effectiveGuiClientId() const
{
    return m_effectiveGuiClientId.isEmpty()
               ? value(QStringLiteral("GUIClientID")).toString()
               : m_effectiveGuiClientId;
}

QString AppSettings::persistentGuiClientId() const { return m_persistentGuiClientId; }
QString AppSettings::effectiveStationName() const { return m_effectiveStationName; }
QString AppSettings::automationIdentity() const { return m_automationIdentity; }
QString AppSettings::automationAgentName() const { return m_automationAgentName; }
bool AppSettings::guiClientIdentityIsTransient() const { return m_guiClientIdentityTransient; }

bool AppSettings::rotatePersistentGuiClientId(const QString& reason)
{
    if (!m_guiClientIdentityInitialized || m_guiClientIdentityTransient) {
        return false;
    }
    m_persistentGuiClientId =
        QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
    m_effectiveGuiClientId = m_persistentGuiClientId;
    QJsonObject identityConfig{
        {QStringLiteral("PersistentId"), m_persistentGuiClientId},
        {QStringLiteral("MachineBinding"), machineBinding()},
    };
    setValue(QString::fromLatin1(kIdentityConfigKey), QString::fromUtf8(
        QJsonDocument(identityConfig).toJson(QJsonDocument::Compact)));
    setValue(QStringLiteral("GUIClientID"), m_persistentGuiClientId);
    save();
    qWarning().noquote() << "AppSettings: rotated GUI client ID —" << reason;
    return true;
}

bool AppSettings::resolveLiveGuiClientIdCollision(const QString& otherStation)
{
    if (!m_guiClientIdentityInitialized) {
        return false;
    }
    if (m_guiClientIdentityTransient) {
        m_effectiveGuiClientId =
            QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
        qWarning().noquote()
            << "AppSettings: live client already owns this process-scoped GUI ID;"
               " selected a collision fallback for station"
            << otherStation;
        return true;
    }
    return rotatePersistentGuiClientId(
        QStringLiteral("live duplicate belongs to station %1").arg(otherStation));
}

void AppSettings::recordPersistentGuiClientIdReply(const QString& clientId)
{
    // Defensive: registerAsGuiClient always sends `client gui <id>` with our
    // non-empty effective id, so the radio normally echoes that same id and we
    // return early below. This only adopts-and-persists if the radio ever
    // hands back a *different* id (e.g. a future firmware that normalizes it),
    // keeping our stored identity aligned with what the radio actually bound.
    if (m_guiClientIdentityTransient) {
        return;
    }
    const QString normalized = validUuidOrEmpty(clientId);
    if (normalized.isEmpty() || normalized == m_persistentGuiClientId) {
        return;
    }
    m_persistentGuiClientId = normalized;
    m_effectiveGuiClientId = normalized;
    QJsonObject identityConfig{
        {QStringLiteral("PersistentId"), normalized},
        {QStringLiteral("MachineBinding"), machineBinding()},
    };
    setValue(QString::fromLatin1(kIdentityConfigKey), QString::fromUtf8(
        QJsonDocument(identityConfig).toJson(QJsonDocument::Compact)));
    setValue(QStringLiteral("GUIClientID"), normalized);
    save();
}

// ─── Migration from QSettings ─────────────────────────────────────────────────

void AppSettings::migrateFromQSettings()
{
    std::unique_ptr<QSettings> old;
    if (QStandardPaths::isTestModeEnabled()) {
        const QString isolatedLegacyPath =
            QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
            + QStringLiteral("/AetherSDR/legacy-qsettings.ini");
        old = std::make_unique<QSettings>(isolatedLegacyPath, QSettings::IniFormat);
    } else {
        old = std::make_unique<QSettings>(QStringLiteral("AetherSDR"),
                                          QStringLiteral("AetherSDR"));
    }
    const QStringList keys = old->allKeys();

    if (keys.isEmpty()) {
        // No old settings — first launch. Set defaults.
        setValue("ApplicationVersion", QCoreApplication::applicationVersion());
        setValue("AutoConnect", "True");
        setValue("AutoConnectToLastRadio", "True");
        setValue("StationName", "");
        setValue("GUIClientID", QUuid::createUuid().toString(QUuid::WithoutBraces));
        setValue("IsSingleClickTuneEnabled", "False");
        setValue("IsSpotsEnabled", "True");
        setValue("PassiveSpotsMode", "False");
        setValue("SpotsMaxLevel", "3");
        setValue("SpotFontSize", "16");
        setValue("FavoriteMode0", "USB");
        setValue("FavoriteMode1", "CW");
        setValue("FavoriteMode2", "AM");
        save();
        qDebug() << "AppSettings: created new settings store with defaults";
        return;
    }

    qDebug() << "AppSettings: migrating" << keys.size() << "keys from QSettings";

    // Map old QSettings keys to new keys
    // MainWindow
    if (old->contains("lastRadioSerial"))
        setValue("LastConnectedRadioSerial", old->value("lastRadioSerial").toString());
    if (old->contains("geometry"))
        setValue("MainWindowGeometry", old->value("geometry").toByteArray().toBase64());
    if (old->contains("windowState"))
        setValue("MainWindowState", old->value("windowState").toByteArray().toBase64());
    if (old->contains("splitterState"))
        setValue("SplitterState", old->value("splitterState").toByteArray().toBase64());

    // Spectrum
    if (old->contains("spectrum/splitRatio"))
        setValue("SpectrumSplitRatio", old->value("spectrum/splitRatio").toString());

    // Spots
    if (old->contains("spots/enabled"))
        setValue("IsSpotsEnabled", old->value("spots/enabled").toBool() ? "True" : "False");
    if (old->contains("spots/levels"))
        setValue("SpotsMaxLevel", old->value("spots/levels").toString());
    if (old->contains("spots/position"))
        setValue("SpotsStartingHeightPercentage", old->value("spots/position").toString());
    if (old->contains("spots/fontSize"))
        setValue("SpotFontSize", old->value("spots/fontSize").toString());
    if (old->contains("spots/overrideColors"))
        setValue("IsSpotsOverrideColorsEnabled",
                 old->value("spots/overrideColors").toBool() ? "True" : "False");
    if (old->contains("spots/overrideBg"))
        setValue("IsSpotsOverrideBackgroundColorsEnabled",
                 old->value("spots/overrideBg").toBool() ? "True" : "False");
    if (old->contains("spots/overrideBgAuto"))
        setValue("IsSpotsOverrideToAutoBackgroundColorEnabled",
                 old->value("spots/overrideBgAuto").toBool() ? "True" : "False");

    // Set defaults for new keys
    setValue("ApplicationVersion", QCoreApplication::applicationVersion());
    setValue("AutoConnect", "True");
    if (!contains("AutoConnectToLastRadio"))
        setValue("AutoConnectToLastRadio", "True");
    setValue("StationName", "");
    setValue("GUIClientID", QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!contains("IsSingleClickTuneEnabled"))
        setValue("IsSingleClickTuneEnabled", "False");
    if (!contains("PassiveSpotsMode"))
        setValue("PassiveSpotsMode", "False");
    if (!contains("FavoriteMode0")) setValue("FavoriteMode0", "USB");
    if (!contains("FavoriteMode1")) setValue("FavoriteMode1", "CW");
    if (!contains("FavoriteMode2")) setValue("FavoriteMode2", "AM");

    save();
    qDebug() << "AppSettings: migration complete, saved to" << m_filePath;
}

} // namespace AetherSDR
