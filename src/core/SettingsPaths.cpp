#include "SettingsPaths.h"

#include <QStandardPaths>

namespace AetherSDR {
namespace SettingsPaths {

QString configDir()
{
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
