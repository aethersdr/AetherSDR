#pragma once

#include <QString>

namespace AetherSDR {

// Single source of truth for every settings-store path. Backed by
// QStandardPaths::writableLocation(GenericConfigLocation), which needs no
// QCoreApplication and honours setTestModeEnabled() (tests/TestSettingsProfile.h).
namespace SettingsPaths {

// ~/.config/AetherSDR (Linux), ~/Library/Preferences/AetherSDR (macOS),
// %LOCALAPPDATA%/AetherSDR (Windows). Does NOT create the directory.
QString configDir();

// The SQLite settings store: <configDir>/AetherSDR.db
QString databasePath();

// The pre-SQLite XML store (now a frozen snapshot after migration):
// <configDir>/AetherSDR.settings
QString legacyXmlPath();

// Rolling verified backups + pre-reset backups: <configDir>/settings-backups
QString backupsDir();

// Quarantined corrupt stores: <configDir>/settings-quarantine
QString quarantineDir();

} // namespace SettingsPaths

} // namespace AetherSDR
