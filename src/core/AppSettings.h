#pragma once

#include <QString>
#include <QVariant>
#include <QMap>
#include <memory>

class QLockFile;

namespace AetherSDR {

// XML-based application settings, structured to match SmartSDR's SSDR.settings.
// Stored at ~/.config/AetherSDR/AetherSDR.settings (Linux/macOS) or
// %LOCALAPPDATA%/AetherSDR/AetherSDR.settings (Windows).
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

    // Station name (defaults to "AetherSDR").
    QString stationName() const;
    void setStationName(const QString& name);

    // File path for the settings file.
    QString filePath() const { return m_filePath; }

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

    // Migrate from old QSettings (INI format) if XML file doesn't exist yet.
    void migrateFromQSettings();

private:
    AppSettings();
    ~AppSettings();
    AppSettings(const AppSettings&) = delete;
    AppSettings& operator=(const AppSettings&) = delete;

    // One-time path migration: on Windows, Qt 6's ConfigLocation produced a
    // triple-nested path. Move the file to the correct GenericConfigLocation.
    void migrateSettingsPath();

    QString m_filePath;
    QMap<QString, QString> m_settings;          // top-level key=value
    QMap<QString, QString> m_stationSettings;   // per-station key=value
    QString m_stationName{"AetherSDR"};
    int m_loadedCount{0};  // settings count at load time (guard against truncated saves)
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
