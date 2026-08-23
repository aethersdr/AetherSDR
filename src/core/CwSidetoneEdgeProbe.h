#pragma once

#include "LogManager.h"

#include <QtGlobal>

#include <array>
#include <cstdlib>

namespace AetherSDR {

// Bench diagnostic: sample-exact envelope edge capture at the sink boundary.
// Enabled only when AETHER_CW_EDGE_PROBE=1 is in the environment; otherwise a
// single bool test per buffer. Both sidetone sinks feed their rendered float
// buffers through scan(); positions are running SAMPLE indices, so the timing
// this yields is the stream's own clock — no wall-clock jitter, no re-record.
//
// Detection: tone ON at the first sample whose |s| exceeds the threshold;
// tone OFF at the first sample of a quiet run at least kQuietRunSamples long
// (a 600 Hz tone crosses zero every ~40 samples at 48 kHz, so instantaneous
// silence inside a cycle never counts as OFF).
class CwSidetoneEdgeProbe {
public:
    CwSidetoneEdgeProbe()
        : m_enabled(qEnvironmentVariable("AETHER_CW_EDGE_PROBE") == QLatin1String("1")) {}

    void scan(const float* interleaved, int frames)
    {
        if (!m_enabled) { return; }
        for (int i = 0; i < frames; ++i) {
            float a = interleaved[2 * i];
            if (a < 0) a = -a;
            if (!m_tone) {
                if (a > kThreshold) {
                    record(m_samplePos + i, true);
                    m_tone = true;
                    m_quietRun = 0;
                }
            } else {
                if (a > kThreshold) {
                    m_quietRun = 0;
                } else {
                    if (m_quietRun == 0) m_quietStart = m_samplePos + i;
                    if (++m_quietRun >= kQuietRunSamples) {
                        record(m_quietStart, false);
                        m_tone = false;
                        m_quietRun = 0;
                    }
                }
            }
        }
        m_samplePos += frames;
    }

    void dump(const char* tag, int sampleRateHz)
    {
        if (!m_enabled || m_count == 0) { return; }
        qCInfo(lcAudio) << "EDGEPROBE" << tag << "rate=" << sampleRateHz
                        << "edges=" << m_count
                        << (m_count == m_edges.size() ? "(TRUNCATED)" : "");
        for (int i = 0; i < m_count; ++i) {
            qCInfo(lcAudio).nospace()
                << "EDGEPROBE " << tag << ' '
                << m_edges[i].samplePos << (m_edges[i].rising ? " R" : " F");
        }
        m_count = 0;
        m_samplePos = 0;
        m_tone = false;
        m_quietRun = 0;
    }

private:
    struct Edge { qint64 samplePos; bool rising; };

    void record(qint64 pos, bool rising)
    {
        if (m_count < static_cast<int>(m_edges.size()))
            m_edges[m_count++] = {pos, rising};
    }

    static constexpr float kThreshold = 0.02f;
    static constexpr int   kQuietRunSamples = 96;   // 2 ms @ 48 kHz

    std::array<Edge, 8192> m_edges{};
    int     m_count{0};
    qint64  m_samplePos{0};
    qint64  m_quietStart{0};
    int     m_quietRun{0};
    bool    m_tone{false};
    bool    m_enabled{false};
};

} // namespace AetherSDR
