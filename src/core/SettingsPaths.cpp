#include "SettingsPaths.h"

#include <QStandardPaths>
#include <QtGlobal>

namespace AetherSDR {
namespace SettingsPaths {

QString configDir()
{
    // Explicit override, primarily for test isolation: on Windows,
    // QStandardPaths resolves known folders through the shell API and ignores
    // environment redirects, so QStandardPaths::setTestModeEnabled() lands
    // every test run in the same real %LOCALAPPDATA%\qttest directory
    // (PR #4612 review, nigelfenton — demonstrated flaky there, clean on
    // POSIX). TestSettingsProfile sets this to its per-run temp dir; it also
    // makes the --config CLI drivable against an isolated store.
    const QString override = qEnvironmentVariable("AETHER_SETTINGS_DIR");
    if (!override.isEmpty()) {
        return override;
    }
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + QStringLiteral("/AetherSDR");
}

QString databasePath()
{
    return configDir() + QStringLiteral("/AetherSDR.db");
}

QString legacyXmlPath()
{
    return configDir() + QStringLiteral("/AetherSDR.settings");
}

QString backupsDir()
{
    return configDir() + QStringLiteral("/settings-backups");
}

QString quarantineDir()
{
    return configDir() + QStringLiteral("/settings-quarantine");
}

} // namespace SettingsPaths
} // namespace AetherSDR
