#include "WaveformUploadState.h"

#include <limits>

namespace AetherSDR {

void WaveformUploadState::advanceGeneration()
{
    if (m_generation == std::numeric_limits<Generation>::max()) {
        m_generation = 1;
    } else {
        ++m_generation;
    }
}

WaveformUploadState::Generation WaveformUploadState::begin(qint64 totalBytes)
{
    advanceGeneration();
    m_totalBytes = totalBytes > 0 ? totalBytes : 0;
    m_queuedBytes = 0;
    m_acknowledgedBytes = 0;
    m_active = totalBytes > 0;
    return m_generation;
}

void WaveformUploadState::invalidate()
{
    advanceGeneration();
    m_totalBytes = 0;
    m_queuedBytes = 0;
    m_acknowledgedBytes = 0;
    m_active = false;
}

bool WaveformUploadState::isCurrent(Generation generation) const
{
    return m_active && generation == m_generation;
}

bool WaveformUploadState::recordQueued(Generation generation,
                                       qint64 requested,
                                       qint64 accepted)
{
    if (!isCurrent(generation)
        || requested <= 0
        || accepted <= 0
        || accepted > requested
        || accepted > m_totalBytes - m_queuedBytes) {
        return false;
    }
    m_queuedBytes += accepted;
    return true;
}

bool WaveformUploadState::acknowledge(Generation generation, qint64 bytes)
{
    if (!isCurrent(generation)
        || bytes <= 0
        || bytes > m_queuedBytes - m_acknowledgedBytes) {
        return false;
    }
    m_acknowledgedBytes += bytes;
    return true;
}

qint64 WaveformUploadState::nextChunkSize(Generation generation,
                                          qint64 maximum) const
{
    if (!isCurrent(generation) || maximum <= 0) {
        return 0;
    }
    return qMin(maximum, m_totalBytes - m_queuedBytes);
}

bool WaveformUploadState::complete(Generation generation) const
{
    return isCurrent(generation)
        && m_totalBytes > 0
        && m_queuedBytes == m_totalBytes
        && m_acknowledgedBytes == m_totalBytes;
}

} // namespace AetherSDR
