#include "CtyDatParser.h"

#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <algorithm>

namespace MasterSDR {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Strip zone overrides "(xx)" and "[xx]" and leading "=" from a prefix token.
static QString cleanPrefix(const QString& raw)
{
    QString s = raw.trimmed();
    // remove zone overrides like (14) and [14]
    static const QRegularExpression zoneRe(R"(\([^)]*\)|\[[^\]]*\])");
    s.remove(zoneRe);
    s = s.trimmed();
    return s;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool CtyDatParser::loadFromFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    QTextStream ts(&f);
    QStringList lines;
    while (!ts.atEnd())
        lines.append(ts.readLine());
    parse(lines);
    return isLoaded();
}

bool CtyDatParser::loadFromResource(const QString& resourcePath)
{
    QFile f(resourcePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    QTextStream ts(&f);
    QStringList lines;
    while (!ts.atEnd())
        lines.append(ts.readLine());
    parse(lines);
    return isLoaded();
}

// ---------------------------------------------------------------------------
// cty.dat parser
//
// Format:
//   Entity Name:  CQ:  ITU:  Continent:  lat:  lon:  tz:  PrimaryPrefix:
//       alias1,alias2,=EXACT1,=EXACT2;
//
// Alias tokens may have zone overrides in () or [] which we strip.
// Tokens starting with '=' are exact callsign matches.
// ---------------------------------------------------------------------------
void CtyDatParser::parse(const QStringList& lines)
{
    m_exactMatch.clear();
    m_prefixTable.clear();
    m_entityByPrefix.clear();
    m_maxPrefixLen = 0;

    // We accumulate alias lines until we hit a new entity header.
    DxccEntity current;
    bool inEntity = false;
    QString aliasBuffer;

    auto commitEntity = [&]() {
        if (!inEntity || current.primaryPrefix.isEmpty())
            return;

        m_entityByPrefix.insert(current.primaryPrefix, current);

        // Register primary prefix itself
        m_prefixTable.insert(current.primaryPrefix, current.primaryPrefix);
        m_maxPrefixLen = std::max(m_maxPrefixLen, (int)current.primaryPrefix.length());

        // Parse accumulated alias buffer
        QString buf = aliasBuffer;
        buf.remove('\n');
        buf.remove('\r');
        // Remove trailing semicolon
        buf = buf.trimmed();
        if (buf.endsWith(';')) buf.chop(1);

        const QStringList tokens = buf.split(',', Qt::SkipEmptyParts);
        for (const QString& raw : tokens) {
            const QString tok = raw.trimmed();
            if (tok.isEmpty()) continue;

            if (tok.startsWith('=')) {
                // Exact match
                const QString exact = cleanPrefix(tok.mid(1)).toUpper();
                if (!exact.isEmpty())
                    m_exactMatch.insert(exact, current.primaryPrefix);
            } else {
                // Prefix
                const QString pfx = cleanPrefix(tok).toUpper();
                if (!pfx.isEmpty()) {
                    m_prefixTable.insert(pfx, current.primaryPrefix);
                    m_maxPrefixLen = std::max(m_maxPrefixLen, (int)pfx.length());
                }
            }
        }
    };

    // Regex for header line: "Entity Name:  CQ:  ITU:  Cont:  lat:  lon:  tz:  Prefix:"
    // Fields are colon-separated on the first line.
    static const QRegularExpression headerRe(
        R"(^([^:]+):\s*(\d+):\s*(\d+):\s*(\w+):\s*[\d\.\-]+:\s*[\d\.\-]+:\s*[\d\.\-]+:\s*([^:]+):)");

    for (const QString& line : lines) {
        // Header line — doesn't start with whitespace
        if (!line.isEmpty() && line[0] != ' ' && line[0] != '\t') {
            // Commit previous entity
            commitEntity();

            auto m = headerRe.match(line);
            if (!m.hasMatch()) {
                inEntity = false;
                continue;
            }

            current = DxccEntity{};
            current.name         = m.captured(1).trimmed();
            current.cqZone       = m.captured(2).toInt();
            current.ituZone      = m.captured(3).toInt();
            current.continent    = m.captured(4).trimmed();
            current.primaryPrefix = m.captured(5).trimmed().toUpper();
            // Remove trailing slash variants like "3D2/c" -> use as-is (sub-entities get own primary prefix)
            inEntity = true;
            aliasBuffer.clear();
        } else if (inEntity) {
            // Continuation line with aliases
            aliasBuffer += line;
        }
    }
    // Commit the last entity
    commitEntity();
}

// ---------------------------------------------------------------------------
// Callsign resolution
// ---------------------------------------------------------------------------

QString CtyDatParser::resolvePrimaryPrefix(const QString& callsign) const
{
    if (callsign.isEmpty()) return {};
    const QString cs = callsign.toUpper();

    // 1. Exact match first
    if (m_exactMatch.contains(cs))
        return m_exactMatch.value(cs);

    // Strip /P /M /MM /AM portable suffixes — use base call for prefix lookup.
    // But keep /country suffixes (e.g. G3ABC/VK4) — the part after the last /
    // is tried as a prefix override if it's 1-3 chars.
    QString base = cs;
    if (cs.contains('/')) {
        const QStringList parts = cs.split('/');
        if (parts.size() == 2) {
            const QString& suffix = parts[1];
            // If suffix looks like a DXCC prefix (not P/M/MM/AM/QRP)
            if (suffix != "P" && suffix != "M" && suffix != "MM" &&
                suffix != "AM" && suffix != "QRP" && suffix.length() <= 4) {
                // Try the suffix as a prefix
                QString r = resolvePrimaryPrefix(suffix);
                if (!r.isEmpty()) return r;
            }
            base = parts[0];
        } else {
            base = parts[0];
        }
    }

    // 2. Longest-prefix match (try from longest to shortest)
    const int maxLen = std::min(m_maxPrefixLen, (int)base.length());
    for (int len = maxLen; len >= 1; --len) {
        const QString pfx = base.left(len);
        auto it = m_prefixTable.find(pfx);
        if (it != m_prefixTable.end())
            return it.value();
    }

    return {};
}

const DxccEntity* CtyDatParser::entityByPrefix(const QString& primaryPrefix) const
{
    auto it = m_entityByPrefix.find(primaryPrefix.toUpper());
    if (it == m_entityByPrefix.end()) return nullptr;
    return &it.value();
}

} // namespace MasterSDR
