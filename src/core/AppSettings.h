#pragma once

#include <QString>
#include <QVariant>
#include <QMap>

namespace MasterSDR {

// XML-based application settings, structured to match SmartSDR's SSDR.settings.
// Stored at ~/.config/MasterSDR/MasterSDR.settings (Linux/macOS) or
// %LOCALAPPDATA%/MasterSDR/MasterSDR.settings (Windows).
//
// Usage:
//   auto& s = AppSettings::instance();
//   s.setValue("LastConnectedRadioSerial", "2125-1213-8600-8895");
//   QString serial = s.value("LastConnectedRadioSerial").toString();
//   s.save();
//
// Per-station settings (like SSDR's <BIGBOX> section):
//   s.setStationValue("AnalogRXMeterSelection", "S-Meter");
//   QString sel = s.stationValue("AnalogRXMeterSelection", "S-Meter").toString();

class AppSettings {
public:
    static AppSettings& instance();

    // Load settings from disk. Called once at startup.
    void load();

    // Save settings to disk.
    void save();

    // Get/set top-level settings.
    QVariant value(const QString& key, const QVariant& defaultValue = {}) const;
    void setValue(const QString& key, const QVariant& val);
    void remove(const QString& key);
    bool contains(const QString& key) const;

    // Per-station settings (nested under <StationName> element).
    QVariant stationValue(const QString& key, const QVariant& defaultValue = {}) const;
    void setStationValue(const QString& key, const QVariant& val);

    // Station name (defaults to "MasterSDR").
    QString stationName() const;
    void setStationName(const QString& name);

    // File path for the settings file.
    QString filePath() const { return m_filePath; }

    // Clear all loaded settings from memory without writing anything back out.
    void reset();

    // Migrate from old QSettings (INI format) if XML file doesn't exist yet.
    void migrateFromQSettings();

private:
    AppSettings();
    ~AppSettings() = default;
    AppSettings(const AppSettings&) = delete;
    AppSettings& operator=(const AppSettings&) = delete;

    // One-time path migration: on Windows, Qt 6's ConfigLocation produced a
    // triple-nested path. Move the file to the correct GenericConfigLocation.
    void migrateSettingsPath();

    QString m_filePath;
    QMap<QString, QString> m_settings;          // top-level key=value
    QMap<QString, QString> m_stationSettings;   // per-station key=value
    QString m_stationName{"MasterSDR"};
    int m_loadedCount{0};  // settings count at load time (guard against truncated saves)
};

} // namespace MasterSDR
