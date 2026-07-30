#pragma once

#include <QString>

namespace AetherSDR {

// Pre-QApplication settings access (RFC #4603).
//
// Two startup consumers need a setting before QApplication exists and before
// AppSettings::load() runs: the UiScale bootstrap in main.cpp (QT_SCALE_FACTOR
// must be set before the QApplication constructor) and GpuSelector (the RHI
// backend choice). They must NOT construct the AppSettings singleton — its
// load() sequencing (path migration, XML import) belongs to the ordinary
// startup flow.
//
// readValue() reads the SQLite store read-only when it exists, and falls back
// to scanning the legacy XML file — which is exactly the state on the very
// first launch after the upgrade, where the import has not run yet.
namespace SettingsBootstrap {

// Returns the stored value for a top-level settings key, or `defaultValue`
// if no store exists or the key is absent. Never creates any file.
QString readValue(const QString& key, const QString& defaultValue = {});

} // namespace SettingsBootstrap

} // namespace AetherSDR
