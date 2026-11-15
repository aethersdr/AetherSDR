#pragma once

#include <QtGlobal>

namespace AetherSDR {

class WaveformUploadState {
public:
    using Generation = quint64;

    Generation begin(qint64 totalBytes);
    void invalidate();

    bool isCurrent(Generation generation) const;
    bool recordQueued(Generation generation, qint64 requested, qint64 accepted);
    bool acknowledge(Generation generation, qint64 bytes);

    qint64 nextChunkSize(Generation generation, qint64 maximum) const;
    qint64 totalBytes() const { return m_totalBytes; }
    qint64 queuedBytes() const { return m_queuedBytes; }
    qint64 acknowledgedBytes() const { return m_acknowledgedBytes; }
    bool complete(Generation generation) const;

private:
    void advanceGeneration();

    Generation m_generation{0};
    qint64 m_totalBytes{0};
    qint64 m_queuedBytes{0};
    qint64 m_acknowledgedBytes{0};
    bool m_active{false};
};

} // namespace AetherSDR
