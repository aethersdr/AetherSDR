#pragma once

#include <QString>

namespace AetherSDR {

// Single source of truth for every path in the client settings store.
//
// Before RFC #4603 the config-dir logic was hand-duplicated in three places
// (AppSettings constructor, the pre-QApplication UiScale scan in main.cpp,
// and GpuSelector) and the copies had already drifted — the main.cpp copy
// ignored $XDG_CONFIG_HOME. Everything now calls this helper.
//
// QStandardPaths::writableLocation(GenericConfigLocation) is deliberately the
// backing call: it needs no QCoreApplication (it is env/known-folder based on
// all three platforms), and it respects QStandardPaths::setTestModeEnabled(),
// which the test fixture (tests/TestSettingsProfile.h) relies on.
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
