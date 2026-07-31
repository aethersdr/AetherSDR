#pragma once

#include "RadioSettingsScope.h"

#include <QSet>
#include <QString>
#include <QVector>

namespace AetherSDR {

struct BandStackEntry {
    double frequencyMhz{0.0};
    QString mode;
    int filterLow{0};
    int filterHigh{0};
    QString rxAntenna;
    QString txAntenna;
    QString agcMode;
    int agcThreshold{0};
    int audioGain{50};
    bool nbOn{false};
    int nbLevel{50};
    bool nrOn{false};
    int nrLevel{50};
    bool wnbOn{false};
    int wnbLevel{50};
    qint64 createdAtMs{0};  // epoch ms; 0 = legacy entry (never auto-expires)
    bool autoSaved{false};  // true if added by auto-save dwell; false = manual
};

// User frequency bookmarks, stored per radio as ONE feature document —
// (family, serial, "BandStack") in radio_settings (RFC #4603 PR 4). Every
// mutation writes the whole document through immediately (atomic, Principle
// V), so there is no separate save() step anymore and a crash can't lose a
// bookmark the operator just made.
//
// The legacy side file (~/.config/AetherSDR/BandStack.settings) is imported
// LAZILY, one radio at a time, on that radio's first access — because the
// document needs the radio's FAMILY, which only the live scope knows (the
// legacy file keyed sections by serial alone). A migrated section is removed
// from the side file; the file itself is deleted when its last radio section
// has been claimed. Sections for radios that never connect again simply wait
// there, harmlessly. The three panel-wide preferences move to AppSettings
// keys (they were never per-radio) via a one-shot in load().
class BandStackSettings {
public:
    static constexpr int kSchemaVersion = 1;
    static QString featureName() { return QStringLiteral("BandStack"); }

    static BandStackSettings& instance();

    // One-shot migration of the legacy file's panel-wide preferences into
    // AppSettings. Idempotent; call once after AppSettings::load().
    void load();

    QVector<BandStackEntry> entries(const RadioSettingsScope& scope);
    void addEntry(const RadioSettingsScope& scope, const BandStackEntry& entry);
    void removeEntry(const RadioSettingsScope& scope, int index);
    void clearAllEntries(const RadioSettingsScope& scope);
    void clearBandEntries(const RadioSettingsScope& scope, double lowMhz,
                          double highMhz);

    // Remove entries older than maxAgeMs; returns number removed.
    int removeExpiredEntries(const RadioSettingsScope& scope, qint64 maxAgeMs);

    // Panel-wide preferences (AppSettings-backed; setters persist).
    int autoExpiryMinutes() const;
    void setAutoExpiryMinutes(int minutes);
    bool groupByBand() const;
    void setGroupByBand(bool grouped);
    // Auto-save: seconds the active slice must dwell on a frequency before
    // being added to the stack automatically. 0 = disabled.
    int autoSaveDwellSeconds() const;
    void setAutoSaveDwellSeconds(int seconds);

private:
    BandStackSettings();
    BandStackSettings(const BandStackSettings&) = delete;
    BandStackSettings& operator=(const BandStackSettings&) = delete;

    // Legacy side-file section name for a serial (XML element-name rules).
    static QString sanitizeSerial(const QString& serial);

    void ensureScopeMigrated(const RadioSettingsScope& scope);
    QVector<BandStackEntry> readEntries(const RadioSettingsScope& scope);
    bool writeEntries(const RadioSettingsScope& scope,
                      const QVector<BandStackEntry>& entries);

    QString m_legacyFilePath;
    QSet<QString> m_scopesChecked;   // per-process migration memo
};

} // namespace AetherSDR
