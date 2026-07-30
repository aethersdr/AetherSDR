#pragma once

#include <QString>

namespace AetherSDR {

// Secret-safe rendering of the settings store for diagnostics (the support
// bundle's settings section and `AetherSDR --config export`).
//
// The store itself must never contain credentials (RFC #4603 proposal E — they
// live in the OS keychain), so redaction here is defense-in-depth: it protects
// dumps taken from stores written by older builds or third-party forks, and it
// is RECURSIVE — a credential-shaped field nested inside a JSON document value
// (e.g. an api key inside a feature's config object) is redacted even though
// the row's own key looks harmless.
namespace SettingsSanitizer {

// True when a key/field name looks credential-bearing.
bool isSecretKey(const QString& key);

// Redact `value` for display under `key`: whole-value redaction when the key
// is secret-shaped, recursive field redaction when the value is a JSON
// document, verbatim otherwise.
QString redactedValue(const QString& key, const QString& value);

// Full sanitized dump of the loaded AppSettings instance (app + station
// scopes), one "key<TAB>value" per line under "## section" headers. Diagnostic
// output — NOT a restorable backup.
QString dump();

} // namespace SettingsSanitizer

} // namespace AetherSDR
