#include "asr/LivePreviewAssembler.h"

#include <algorithm>

namespace AetherSDR {

void LivePreviewAssembler::reset()
{
    m_closedEnd.clear();
    m_content.clear();
    m_openBlock = 0;
    m_openStart = 0;
    m_resolvedEnd = 0;
    m_anyResolved = false;
    m_lastResolved = 0;
}

std::vector<LivePreviewAssembler::Update> LivePreviewAssembler::closeBlock(quint64 id,
                                                                          qint64 endSample)
{
    std::vector<Update> changed;
    if (m_anyResolved && id <= m_lastResolved) {
        return changed;
    }
    m_closedEnd[id] = endSample;
    if (id < m_openBlock) {
        return changed;
    }
    m_openBlock = id + 1;
    m_openStart = std::max(m_openStart, endSample);

    const auto it = m_content.find(id);
    if (it == m_content.end()) {
        return changed;
    }
    Content& from = it->second;
    Content moved;
    const auto past = from.finals.lower_bound(endSample);
    moved.finals.insert(past, from.finals.end());
    from.finals.erase(past, from.finals.end());
    if (from.provisionalStart >= endSample) {
        moved.provisionalStart = from.provisionalStart;
        moved.provisional = from.provisional;
        from.provisionalStart = -1;
        from.provisional.clear();
    }
    if (moved.finals.empty() && moved.provisionalStart < 0) {
        return changed;
    }
    // Block id + 1 opens only now, so normally it is empty; merge regardless.
    Content& to = m_content[id + 1];
    to.finals.merge(moved.finals);
    if (moved.provisionalStart >= 0 && to.provisionalStart < 0) {
        to.provisionalStart = moved.provisionalStart;
        to.provisional = moved.provisional;
    }
    if (auto update = render(id, from)) {
        changed.push_back(*update);
    }
    if (auto update = render(id + 1, to)) {
        changed.push_back(*update);
    }
    return changed;
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
    if (block == m_openBlock && startSample < m_openStart) {
        return std::nullopt; // in audio the segmenter discarded
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
    return render(block, c);
}

std::optional<LivePreviewAssembler::Update>
LivePreviewAssembler::discardOpenThrough(qint64 endSample)
{
    if (endSample <= m_openStart) {
        return std::nullopt;
    }
    m_openStart = endSample;
    const auto it = m_content.find(m_openBlock);
    if (it == m_content.end()) {
        return std::nullopt;
    }
    Content& c = it->second;
    c.finals.erase(c.finals.begin(), c.finals.lower_bound(endSample));
    if (c.provisionalStart >= 0 && c.provisionalStart < endSample) {
        c.provisionalStart = -1;
        c.provisional.clear();
    }
    return render(m_openBlock, c);
}

std::optional<LivePreviewAssembler::Update> LivePreviewAssembler::render(quint64 block,
                                                                       Content& c)
{
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
