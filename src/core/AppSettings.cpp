#include "AppSettings.h"
#include "GuiClientIdentityPolicy.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QSettings>
#include <QUuid>
#include <QDebug>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSysInfo>

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
    // GenericConfigLocation gives a plain base dir (no org/app suffix) on all
    // platforms: ~/.config on Linux/macOS, %LOCALAPPDATA% on Windows.
    // ConfigLocation on Windows Qt 6 resolves to AppConfigLocation
    // (%LOCALAPPDATA%/OrgName/AppName), so appending "/AetherSDR" produces a
    // triple-nested path. GenericConfigLocation avoids this and stays consistent
    // with the log-dir path used in main.cpp.
    const QString configDir =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + "/AetherSDR";
    QDir().mkpath(configDir);
    m_filePath = configDir + "/AetherSDR.settings";

    migrateSettingsPath();
}

// ─── Path migration ───────────────────────────────────────────────────────────

void AppSettings::migrateSettingsPath()
{
    if (QFile::exists(m_filePath))
        return;  // already at the correct location

#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    // On Windows and macOS, Qt 6's ConfigLocation resolves to AppConfigLocation
    // (%LOCALAPPDATA%/AetherSDR/AetherSDR on Windows,
    //  ~/Library/Preferences/AetherSDR/AetherSDR on macOS), so appending
    // "/AetherSDR" produced a triple-nested path. Move the file if found there.
    const QString oldPath =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
        + "/AetherSDR/AetherSDR.settings";
    if (QFile::exists(oldPath)) {
        if (QFile::rename(oldPath, m_filePath)) {
            qDebug() << "AppSettings: migrated settings from" << oldPath << "to" << m_filePath;
        } else {
            qWarning() << "AppSettings: failed to migrate from" << oldPath << "to" << m_filePath;
        }
    }
#endif
}

// ─── Load ─────────────────────────────────────────────────────────────────────

void AppSettings::load()
{
    m_loadState = LoadState::Failed;
    m_preserveRecoveryBackup = false;
    m_loadedCount = 0;
    m_settings.clear();
    m_stationSettings.clear();
    m_stationName = QStringLiteral("AetherSDR");

    const QString tmpPath = m_filePath + QStringLiteral(".tmp");
    const QString bakPath = m_filePath + QStringLiteral(".bak");

    if (!QFile::exists(m_filePath)) {
        QString recoveryError;

        // A validated temp file is the newest intended state after a crash in
        // the save window between live->backup and temp->live. Promote it only
        // when the live path is absent, so a valid committed live file always
        // remains authoritative.
        if (QFile::exists(tmpPath)
            && readSettingsFile(tmpPath, m_settings, m_stationSettings,
                                m_stationName, recoveryError)) {
            if (QFile::rename(tmpPath, m_filePath)) {
                QFile::setPermissions(m_filePath,
                                      QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                m_preserveRecoveryBackup = QFile::exists(bakPath);
                m_loadState = LoadState::ReadyToSave;
                m_loadedCount = m_settings.size();
                qDebug() << "AppSettings: promoted interrupted commit with"
                         << m_settings.size() << "settings";
                return;
            }
            qWarning() << "AppSettings: valid temporary settings could not be promoted";
            m_settings.clear();
            m_stationSettings.clear();
            m_stationName = QStringLiteral("AetherSDR");
        } else if (QFile::exists(tmpPath)) {
            qWarning() << "AppSettings: temporary recovery file is corrupt:"
                       << recoveryError;
        }

        if (QFile::exists(bakPath)
            && readSettingsFile(bakPath, m_settings, m_stationSettings,
                                m_stationName, recoveryError)) {
            m_preserveRecoveryBackup = true;
            m_loadState = LoadState::ReadyToSave;
            m_loadedCount = m_settings.size();
            qDebug() << "AppSettings: recovered missing live profile from backup with"
                     << m_settings.size() << "settings";
            return;
        }

        // Recovery artifacts prove this is not a true first launch. If none is
        // usable, stay failed so startup code cannot replace them with defaults.
        if (QFile::exists(tmpPath) || QFile::exists(bakPath)) {
            if (QFile::exists(bakPath)) {
                qWarning() << "AppSettings: backup recovery file is unavailable or corrupt:"
                           << recoveryError;
            }
            m_settings.clear();
            m_stationSettings.clear();
            m_stationName = QStringLiteral("AetherSDR");
            return;
        }

        // No live or recovery artifacts: this is a genuine first launch or a
        // legacy QSettings migration.
        m_loadState = LoadState::ReadyToSave;
        migrateFromQSettings();
        m_loadedCount = m_settings.size();
        return;
    }

    QString error;
    if (!readSettingsFile(m_filePath, m_settings, m_stationSettings,
                          m_stationName, error)) {
        qWarning() << "AppSettings: cannot load" << m_filePath << ':' << error;

        if (!QFile::exists(bakPath)
            || !readSettingsFile(bakPath, m_settings, m_stationSettings,
                                 m_stationName, error)) {
            if (QFile::exists(bakPath)) {
                qWarning() << "AppSettings: backup also unavailable or corrupt:" << error;
            }
            m_settings.clear();
            m_stationSettings.clear();
            m_stationName = QStringLiteral("AetherSDR");
            return;
        }

        m_preserveRecoveryBackup = true;
        qDebug() << "AppSettings: recovered" << m_settings.size()
                 << "settings from backup";
    }

    m_loadState = LoadState::ReadyToSave;
    m_loadedCount = m_settings.size();
    qDebug() << "AppSettings: loaded" << m_settings.size() << "settings +"
             << m_stationSettings.size() << "station settings from" << m_filePath;
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

    if (m_loadState != LoadState::ReadyToSave) {
        qWarning() << "AppSettings: refusing to save before a successful load";
        return;
    }

    // Guard: refuse to save if we'd lose more than half the settings.
    // This catches cases where the app crashes early or settings were
    // cleared from memory before save() runs.
    if (m_loadedCount > 20 && m_settings.size() < m_loadedCount / 2) {
        qWarning() << "AppSettings: refusing to save — only" << m_settings.size()
                   << "settings, loaded" << m_loadedCount << "(would lose data)";
        return;
    }

    // Atomic save: write to temp file, then rename over the original.
    // This prevents data loss if the app crashes or is killed mid-write.
    const QString tmpPath = m_filePath + QStringLiteral(".tmp");
    const QString bakPath = m_filePath + QStringLiteral(".bak");
    QFile file(tmpPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "AppSettings: cannot write" << tmpPath;
        return;
    }

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.setAutoFormattingIndent(2);
    xml.writeStartDocument();
    xml.writeStartElement("Settings");

    // Write top-level settings (sorted for consistency)
    QList<QString> keys = m_settings.keys();
    std::sort(keys.begin(), keys.end());
    // XML element names: start with letter or underscore, then letters/digits/underscores.
    // '/' and '.' are NOT valid in element names — reject them here so a bad key
    // never silently corrupts the file and causes the atomic-save validation to abort.
    static const QRegularExpression validKey("^[A-Za-z_][A-Za-z0-9_]*$");
    for (const auto& key : keys) {
        if (!validKey.match(key).hasMatch()) {
            qWarning() << "AppSettings: skipping key with invalid XML characters:" << key;
            continue;
        }
        xml.writeTextElement(key, m_settings.value(key));
    }

    // Write per-station section
    if (!m_stationSettings.isEmpty()) {
        xml.writeStartElement(m_stationName);
        QList<QString> stKeys = m_stationSettings.keys();
        std::sort(stKeys.begin(), stKeys.end());
        for (const auto& key : stKeys) {
            xml.writeTextElement(key, m_stationSettings.value(key));
        }
        xml.writeEndElement(); // station
    }

    xml.writeEndElement(); // Settings
    xml.writeEndDocument();
    file.close();

    // Validate the temp file before committing — re-read and parse it.
    // If parsing fails, the file is corrupt; don't overwrite the original.
    {
        QFile check(tmpPath);
        if (!check.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qWarning() << "AppSettings: cannot reopen temp file for validation — NOT saving";
            QFile::remove(tmpPath);
            return;
        }
        QXmlStreamReader validator(&check);
        while (!validator.atEnd()) {
            validator.readNext();
        }
        check.close();
        if (validator.hasError()) {
            qWarning() << "AppSettings: temp file failed validation:"
                       << validator.errorString() << "— NOT saving";
            QFile::remove(tmpPath);
            return;
        }
    }

    // Atomic rename: on Linux/macOS this is a single inode swap.
    // On Windows, QFile::rename fails if the target exists, so rotate first.
    QString displacedCorruptPath;
    bool rotatedLiveToBackup = false;
    if (QFile::exists(m_filePath)) {
        if (m_preserveRecoveryBackup && QFile::exists(bakPath)) {
            // Keep the known-good recovery source through the first save after
            // recovery. Move the existing live file aside only until the
            // replacement has been installed.
            displacedCorruptPath = m_filePath + QStringLiteral(".corrupt");
            QFile::remove(displacedCorruptPath);
            if (!QFile::rename(m_filePath, displacedCorruptPath)) {
                qWarning() << "AppSettings: cannot move corrupt live file aside";
                QFile::remove(tmpPath);
                return;
            }
        } else {
            QFile::remove(bakPath);
            if (!QFile::rename(m_filePath, bakPath)) {
                qWarning() << "AppSettings: cannot rotate live settings to backup";
                QFile::remove(tmpPath);
                return;
            }
            rotatedLiveToBackup = true;
            QFile::setPermissions(bakPath,
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        }
    }
    if (!QFile::rename(tmpPath, m_filePath)) {
        qWarning() << "AppSettings: atomic rename failed from" << tmpPath << "to" << m_filePath;
        if (!displacedCorruptPath.isEmpty()) {
            QFile::rename(displacedCorruptPath, m_filePath);
        } else if (rotatedLiveToBackup && !QFile::exists(m_filePath)) {
            if (!QFile::copy(bakPath, m_filePath)) {
                qWarning() << "AppSettings: could not restore live settings from backup";
            } else {
                QFile::setPermissions(m_filePath,
                                      QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            }
        }
        return;
    }

    if (!displacedCorruptPath.isEmpty()) {
        QFile::remove(displacedCorruptPath);
    }

    // Restrict permissions to owner read/write (mode 0600).  Default umask
    // leaves these XML files world-readable, exposing MQTT broker creds,
    // SmartLink email, radio nicknames, etc.  Matches AsyncLogWriter
    // precedent.  See GHSA-mmqp-cm4w-cvpp (L5).
    QFile::setPermissions(m_filePath,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    m_loadedCount = m_settings.size();
    m_preserveRecoveryBackup = false;
}

void AppSettings::reset()
{
    m_guiClientLock.reset();
    m_settings.clear();
    m_stationSettings.clear();
    m_stationName = "AetherSDR";
    m_loadedCount = 0;
    m_loadState = LoadState::NotAttempted;
    m_preserveRecoveryBackup = false;
    m_persistentGuiClientId.clear();
    m_effectiveGuiClientId.clear();
    m_effectiveStationName.clear();
    m_automationIdentity.clear();
    m_automationAgentName.clear();
    m_guiClientIdentityInitialized = false;
    m_guiClientIdentityTransient = false;
}

// ─── Top-level accessors ──────────────────────────────────────────────────────

QVariant AppSettings::value(const QString& key, const QVariant& defaultValue) const
{
    if (m_settings.contains(key))
        return m_settings.value(key);
    return defaultValue;
}

void AppSettings::setValue(const QString& key, const QVariant& val)
{
    m_settings.insert(key, val.toString());
}

void AppSettings::remove(const QString& key)
{
    m_settings.remove(key);
}

bool AppSettings::contains(const QString& key) const
{
    return m_settings.contains(key);
}

// ─── Per-station accessors ────────────────────────────────────────────────────

QVariant AppSettings::stationValue(const QString& key, const QVariant& defaultValue) const
{
    if (m_stationSettings.contains(key))
        return m_stationSettings.value(key);
    return defaultValue;
}

void AppSettings::setStationValue(const QString& key, const QVariant& val)
{
    m_stationSettings.insert(key, val.toString());
}

QString AppSettings::stationName() const
{
    return m_stationName;
}

void AppSettings::setStationName(const QString& name)
{
    m_stationName = name;
    m_settings.insert("StationName", name);
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
    // instance doesn't rewrite the file. The one racy case — two fresh installs
    // with no stored id launched simultaneously — writes divergent persistent
    // ids last-writer-wins, but each instance's *effective* id stays distinct
    // via the lock below, so runtime behavior is unaffected.
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
        qDebug() << "AppSettings: created new settings file with defaults";
        return;
    }

    qDebug() << "AppSettings: migrating" << keys.size() << "keys from QSettings";

    // Map old QSettings keys to new XML keys
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
