#pragma once

#include <QByteArray>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>

// Must be constructed before QApplication/QCoreApplication and before the
// first AppSettings or ThemeManager access.  Qt does not honor the same config
// environment variable on every supported platform, so redirect all relevant
// homes and enable QStandardPaths test mode as a second layer.
class TestSettingsProfile
{
public:
    explicit TestSettingsProfile(const QString& testName)
        : m_root(QDir::tempPath() + QStringLiteral("/") + testName
                 + QStringLiteral("-XXXXXX"))
    {
        if (!m_root.isValid()) {
            return;
        }

        const QByteArray root = m_root.path().toUtf8();
        qputenv("HOME", root);
        qputenv("CFFIXED_USER_HOME", root);
        qputenv("XDG_CONFIG_HOME", root);
        qputenv("LOCALAPPDATA", root);
        qputenv("APPDATA", root);
        // On Windows, QStandardPaths ignores the env redirects above (known
        // folders resolve through the shell API), so test-mode paths land in
        // the REAL shared %LOCALAPPDATA%\qttest and scenarios contaminate
        // each other (PR #4612 review). AETHER_SETTINGS_DIR is the explicit
        // settings-store override SettingsPaths honors on every platform —
        // the settings sandbox no longer depends on platform env semantics.
        qputenv("AETHER_SETTINGS_DIR",
                QString(m_root.path() + QStringLiteral("/AetherSDR")).toUtf8());
        QStandardPaths::setTestModeEnabled(true);

        // AppSettings first-run migration still probes the legacy QSettings
        // store. On macOS the native CFPreferences backend can ignore HOME,
        // so force that probe into a private INI directory as well.
        const QString legacyRoot = m_root.path() + QStringLiteral("/legacy-settings");
        QDir().mkpath(legacyRoot);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, legacyRoot);
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, legacyRoot);
    }

    bool isValid() const { return m_root.isValid(); }
    QString path() const { return m_root.path(); }

private:
    QTemporaryDir m_root;
};
