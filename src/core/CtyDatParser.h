#pragma once

#include <QString>
#include <QHash>
#include <QSet>

namespace MasterSDR {

struct DxccEntity {
    QString primaryPrefix;   // e.g. "G"
    QString name;            // e.g. "England"
    QString continent;       // e.g. "EU"
    int     cqZone{0};
    int     ituZone{0};
};

// ---------------------------------------------------------------------------
// CtyDatParser
//
// Parses the AD1C cty.dat file and resolves callsigns to DXCC entities via
// longest-prefix matching.  Load once at startup; queries are O(prefix_len).
// ---------------------------------------------------------------------------
class CtyDatParser {
public:
    // Returns true on success.
    bool loadFromFile(const QString& path);
    bool loadFromResource(const QString& resourcePath);   // e.g. ":/cty.dat"

    // Resolve callsign -> primary prefix of matched entity ("G", "VK", …)
    // Returns empty string if no match.
    QString resolvePrimaryPrefix(const QString& callsign) const;

    // Look up entity details by primary prefix.
    const DxccEntity* entityByPrefix(const QString& primaryPrefix) const;

    int entityCount() const { return m_entityByPrefix.size(); }
    bool isLoaded()   const { return !m_entityByPrefix.isEmpty(); }

private:
    void parse(const QStringList& lines);

    // exact-match table:  "=VK9XX"  -> primaryPrefix
    QHash<QString, QString> m_exactMatch;
    // prefix table: "VK9X" -> primaryPrefix
    QHash<QString, QString> m_prefixTable;
    // entity details keyed by primaryPrefix
    QHash<QString, DxccEntity> m_entityByPrefix;
    int m_maxPrefixLen{0};
};

} // namespace MasterSDR
