#pragma once

#include <QString>

namespace AetherSDR {

// Pre-QApplication settings access (RFC #4603) for the UiScale bootstrap
// (QT_SCALE_FACTOR) and GpuSelector. Must not construct AppSettings, whose
// load() does path migration and XML import. readValue() reads SQLite
// read-only if present, else scans the legacy XML (first launch after upgrade).
namespace SettingsBootstrap {

// Returns the stored value for a top-level settings key, or `defaultValue`
// if no store exists or the key is absent. Never creates any file.
QString readValue(const QString& key, const QString& defaultValue = {});

} // namespace SettingsBootstrap

} // namespace AetherSDR
