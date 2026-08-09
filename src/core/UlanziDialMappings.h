#pragma once

#include <QString>
#include <QStringList>

namespace AetherSDR {

// Owner of the Ulanzi Dial's pill→action bindings — Constitution Principle V:
// one feature-owned document, one owner, one migration point.  The bindings
// live in a single JSON object under the app-global `UlanziDialMappings` key
// (they belong to the peripheral, not to any radio, so this is correctly a
// flat key rather than a `radio_settings` feature document).
//
// The document is the sole authority once `migrateLegacyKeys()` has run: a
// pill with no entry means "use the built-in default", including after the
// operator deliberately clears one.  Nothing consults the pre-#4611 flat keys
// on the read path, so a cleared binding cannot be resurrected by them.
//
// Kept out of the mapper dialog so it is testable without a GUI stack
// (ulanzi_mapping_migration_test).
class UlanziDialMappings {
public:
    // AppSettings key holding the whole JSON document.
    static QString rootSettingsKey();

    // Bound action for a pill, or an empty string when the document has no
    // entry (the caller supplies its own built-in default).
    static QString actionForPill(const QString& pillId);

    // Bind an action and persist immediately.  Writes the whole document —
    // a whole-document write IS the atomic feature update.  Returns false and
    // logs when the value did not reach the database FILE (a failed save()
    // keeps the change in memory for retry, so only a disk read proves it).
    static bool setActionForPill(const QString& pillId, const QString& actionId);

    // One-shot claim-and-freeze of the pre-#4611 flat keys, run once at
    // feature startup rather than on every access (AGENTS.md "Settings
    // Migration").  For each pill the document does not already describe, a
    // legacy value is adopted into the document; every legacy key is then
    // removed. Returns the number of bindings adopted.
    static int migrateLegacyKeys(const QStringList& pillIds);

    // Bound rotary action for the dial wheel, or "WheelFrequency" by default.
    static QString rotaryAction();

    // Bind rotary action and persist immediately inside the UlanziDialMappings root document.
    static bool setRotaryAction(const QString& actionId);

    // Returns the complete list of wheel action IDs handled by the wheel dispatch chain.
    static const QStringList& knownWheelActions();

    // True if actionId is a recognized wheel action handled by the wheel dispatch chain.
    static bool isKnownWheelAction(const QString& actionId);

    // Pre-#4611 per-pill key names, retained only as migration sources.
    static QString legacyUnderscoreKey(const QString& pillId);
    static QString legacySlashKey(const QString& pillId);
};

}  // namespace AetherSDR
