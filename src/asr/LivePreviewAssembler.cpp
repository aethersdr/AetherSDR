#include "asr/LivePreviewAssembler.h"

#include <algorithm>

namespace AetherSDR {

void LivePreviewAssembler::reset()
{
    m_closedEnd.clear();
    m_content.clear();
    m_openBlock = 0;
    m_resolvedEnd = 0;
    m_anyResolved = false;
    m_lastResolved = 0;
}

void LivePreviewAssembler::closeBlock(quint64 id, qint64 endSample)
{
    if (m_anyResolved && id <= m_lastResolved) {
        return;
    }
    m_closedEnd[id] = endSample;
    if (id >= m_openBlock) {
        m_openBlock = id + 1;
    }
}

std::optional<LivePreviewAssembler::Update>
LivePreviewAssembler::addResult(qint64 startSample, qint64 endSample, const QString& text,
                                bool isFinal)
{
    if (startSample < m_resolvedEnd) {
        return std::nullopt;
    }
    quint64 block = m_openBlock;
    for (const auto& [id, end] : m_closedEnd) {
        if (startSample < end) {
            block = id;
            break;
        }
    }

    Content& c = m_content[block];
    if (isFinal) {
        c.finals[startSample] = text;
        // A final supersedes the provisional text it covers.
        if (c.provisionalStart >= startSample && c.provisionalStart < endSample) {
            c.provisionalStart = -1;
            c.provisional.clear();
        }
    } else {
        c.provisionalStart = startSample;
        c.provisional = text;
    }

    // In audio order: the provisional piece sits wherever its start falls.
    QString joined;
    bool provisionalPlaced = c.provisionalStart < 0;
    for (const auto& [start, t] : c.finals) {
        if (!provisionalPlaced && c.provisionalStart < start) {
            joined += c.provisional + QLatin1Char(' ');
            provisionalPlaced = true;
        }
        joined += t + QLatin1Char(' ');
    }
    if (!provisionalPlaced) {
        joined += c.provisional;
    }
    joined = joined.simplified();
    if (joined == c.shown) {
        return std::nullopt;
    }
    c.shown = joined;
    return Update{block, joined};
}

void LivePreviewAssembler::resolve(quint64 id)
{
    const auto end = m_closedEnd.find(id);
    if (end != m_closedEnd.end()) {
        m_resolvedEnd = std::max(m_resolvedEnd, end->second);
    }
    m_closedEnd.erase(m_closedEnd.begin(), m_closedEnd.upper_bound(id));
    m_content.erase(m_content.begin(), m_content.upper_bound(id));
    m_anyResolved = true;
    m_lastResolved = std::max(m_lastResolved, id);
    if (m_openBlock <= id) {
        m_openBlock = id + 1;
    }
}

} // namespace AetherSDR
