#pragma once

#include <QString>
#include <QVector>
#include <QMap>

namespace MasterSDR {

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

// Persistence for user frequency bookmarks, stored per-radio in
// ~/.config/MasterSDR/BandStack.settings (XML, atomic save).
class BandStackSettings {
public:
    static BandStackSettings& instance();

    void load();
    void save();

    QVector<BandStackEntry> entries(const QString& radioSerial) const;
    void addEntry(const QString& radioSerial, const BandStackEntry& entry);
    void removeEntry(const QString& radioSerial, int index);
    void clearAllEntries(const QString& radioSerial);
    void clearBandEntries(const QString& radioSerial, double lowMhz, double highMhz);

    // Remove entries older than maxAgeMs; returns number removed.
    int removeExpiredEntries(const QString& radioSerial, qint64 maxAgeMs);

    int autoExpiryMinutes() const { return m_autoExpiryMinutes; }
    void setAutoExpiryMinutes(int minutes) { m_autoExpiryMinutes = minutes; }

    bool groupByBand() const { return m_groupByBand; }
    void setGroupByBand(bool grouped) { m_groupByBand = grouped; }

    // Auto-save: seconds the active slice must dwell on a frequency before
    // being added to the stack automatically.  0 = disabled.
    int autoSaveDwellSeconds() const { return m_autoSaveDwellSeconds; }
    void setAutoSaveDwellSeconds(int seconds) { m_autoSaveDwellSeconds = seconds; }

private:
    BandStackSettings();
    BandStackSettings(const BandStackSettings&) = delete;
    BandStackSettings& operator=(const BandStackSettings&) = delete;

    static QString sanitizeSerial(const QString& serial);

    QMap<QString, QVector<BandStackEntry>> m_entries;
    QString m_filePath;
    int m_autoExpiryMinutes{0};      // 0 = disabled
    bool m_groupByBand{false};
    int m_autoSaveDwellSeconds{0};   // 0 = disabled
};

} // namespace MasterSDR
