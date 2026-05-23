#include "AppSettings.h"

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

namespace MasterSDR {

AppSettings& AppSettings::instance()
{
    static AppSettings s;
    return s;
}

AppSettings::AppSettings()
{
    // GenericConfigLocation gives a plain base dir (no org/app suffix) on all
    // platforms: ~/.config on Linux/macOS, %LOCALAPPDATA% on Windows.
    // ConfigLocation on Windows Qt 6 resolves to AppConfigLocation
    // (%LOCALAPPDATA%/OrgName/AppName), so appending "/MasterSDR" produces a
    // triple-nested path. GenericConfigLocation avoids this and stays consistent
    // with the log-dir path used in main.cpp.
    const QString configDir =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + "/MasterSDR";
    QDir().mkpath(configDir);
    m_filePath = configDir + "/MasterSDR.settings";

    migrateSettingsPath();
}

// ─── Path migration ───────────────────────────────────────────────────────────

void AppSettings::migrateSettingsPath()
{
    if (QFile::exists(m_filePath))
        return;  // already at the correct location

#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    // On Windows and macOS, Qt 6's ConfigLocation resolves to AppConfigLocation
    // (%LOCALAPPDATA%/MasterSDR/MasterSDR on Windows,
    //  ~/Library/Preferences/MasterSDR/MasterSDR on macOS), so appending
    // "/MasterSDR" produced a triple-nested path. Move the file if found there.
    const QString oldPath =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
        + "/MasterSDR/MasterSDR.settings";
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
    QFile file(m_filePath);
    if (!file.exists()) {
        // First launch or migration needed
        migrateFromQSettings();
        return;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "AppSettings: cannot open" << m_filePath;
        return;
    }

    m_settings.clear();
    m_stationSettings.clear();

    QXmlStreamReader xml(&file);
    QString currentStation;  // non-empty when inside a station element

    while (!xml.atEnd()) {
        xml.readNext();

        if (xml.isStartElement()) {
            const QString tag = xml.name().toString();

            if (tag == "Settings") continue;  // root element

            // Check if this is the station element
            if (tag == m_stationName && currentStation.isEmpty()) {
                currentStation = tag;
                continue;
            }

            // Read the text content
            const QString text = xml.readElementText();

            if (!currentStation.isEmpty()) {
                m_stationSettings.insert(tag, text);
            } else {
                m_settings.insert(tag, text);
                // Track station name
                if (tag == "StationName")
                    m_stationName = text;
            }
        } else if (xml.isEndElement()) {
            if (xml.name().toString() == m_stationName && !currentStation.isEmpty())
                currentStation.clear();
        }
    }

    if (xml.hasError()) {
        qWarning() << "AppSettings: XML parse error:" << xml.errorString();

        // Try to recover from backup if the main file is corrupt
        const QString bakPath = m_filePath + ".bak";
        if (QFile::exists(bakPath) && m_settings.size() < 10) {
            qWarning() << "AppSettings: main file corrupt, recovering from backup";
            file.close();
            m_settings.clear();
            m_stationSettings.clear();

            QFile bakFile(bakPath);
            if (bakFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QXmlStreamReader bakXml(&bakFile);
                QString bakStation;
                while (!bakXml.atEnd()) {
                    bakXml.readNext();
                    if (bakXml.isStartElement()) {
                        const QString tag = bakXml.name().toString();
                        if (tag == "Settings") continue;
                        if (tag == m_stationName && bakStation.isEmpty()) {
                            bakStation = tag;
                            continue;
                        }
                        const QString text = bakXml.readElementText();
                        if (!bakStation.isEmpty())
                            m_stationSettings.insert(tag, text);
                        else {
                            m_settings.insert(tag, text);
                            if (tag == "StationName") m_stationName = text;
                        }
                    } else if (bakXml.isEndElement()) {
                        if (bakXml.name().toString() == m_stationName && !bakStation.isEmpty())
                            bakStation.clear();
                    }
                }
                bakFile.close();
                if (!bakXml.hasError())
                    qDebug() << "AppSettings: recovered" << m_settings.size() << "settings from backup";
                else
                    qWarning() << "AppSettings: backup also corrupt:" << bakXml.errorString();
            }
        }
    }

    file.close();
    m_loadedCount = m_settings.size();
    qDebug() << "AppSettings: loaded" << m_settings.size() << "settings +"
             << m_stationSettings.size() << "station settings from" << m_filePath;
}

// ─── Save ─────────────────────────────────────────────────────────────────────

void AppSettings::save()
{
    if (QCoreApplication* app = QCoreApplication::instance()) {
        if (app->property("MasterSettingsResetInProgress").toBool()) {
            qWarning() << "AppSettings: skipping save during reset-triggered shutdown";
            return;
        }
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
    const QString tmpPath = m_filePath + ".tmp";

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
        if (check.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QXmlStreamReader validator(&check);
            while (!validator.atEnd()) validator.readNext();
            check.close();
            if (validator.hasError()) {
                qWarning() << "AppSettings: temp file failed validation:"
                           << validator.errorString() << "— NOT saving";
                QFile::remove(tmpPath);
                return;
            }
        }
    }

    // Atomic rename: on Linux/macOS this is a single inode swap.
    // On Windows, QFile::rename fails if the target exists, so remove first.
    if (QFile::exists(m_filePath)) {
        // Keep a backup in case something goes wrong
        const QString bakPath = m_filePath + ".bak";
        QFile::remove(bakPath);
        QFile::rename(m_filePath, bakPath);
    }
    if (!QFile::rename(tmpPath, m_filePath)) {
        qWarning() << "AppSettings: atomic rename failed from" << tmpPath << "to" << m_filePath;
    }
}

void AppSettings::reset()
{
    m_settings.clear();
    m_stationSettings.clear();
    m_stationName = "MasterSDR";
    m_loadedCount = 0;
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

// ─── Migration from QSettings ─────────────────────────────────────────────────

void AppSettings::migrateFromQSettings()
{
    QSettings old("MasterSDR", "MasterSDR");
    const QStringList keys = old.allKeys();

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
    if (old.contains("lastRadioSerial"))
        setValue("LastConnectedRadioSerial", old.value("lastRadioSerial").toString());
    if (old.contains("geometry"))
        setValue("MainWindowGeometry", old.value("geometry").toByteArray().toBase64());
    if (old.contains("windowState"))
        setValue("MainWindowState", old.value("windowState").toByteArray().toBase64());
    if (old.contains("splitterState"))
        setValue("SplitterState", old.value("splitterState").toByteArray().toBase64());

    // Spectrum
    if (old.contains("spectrum/splitRatio"))
        setValue("SpectrumSplitRatio", old.value("spectrum/splitRatio").toString());

    // Spots
    if (old.contains("spots/enabled"))
        setValue("IsSpotsEnabled", old.value("spots/enabled").toBool() ? "True" : "False");
    if (old.contains("spots/levels"))
        setValue("SpotsMaxLevel", old.value("spots/levels").toString());
    if (old.contains("spots/position"))
        setValue("SpotsStartingHeightPercentage", old.value("spots/position").toString());
    if (old.contains("spots/fontSize"))
        setValue("SpotFontSize", old.value("spots/fontSize").toString());
    if (old.contains("spots/overrideColors"))
        setValue("IsSpotsOverrideColorsEnabled",
                 old.value("spots/overrideColors").toBool() ? "True" : "False");
    if (old.contains("spots/overrideBg"))
        setValue("IsSpotsOverrideBackgroundColorsEnabled",
                 old.value("spots/overrideBg").toBool() ? "True" : "False");
    if (old.contains("spots/overrideBgAuto"))
        setValue("IsSpotsOverrideToAutoBackgroundColorEnabled",
                 old.value("spots/overrideBgAuto").toBool() ? "True" : "False");

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

} // namespace MasterSDR
