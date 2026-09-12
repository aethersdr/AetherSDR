#pragma once

#include "LogManager.h"

#include <QtGlobal>

#include <array>
#include <cstdlib>
#include <memory>

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
    // The 128 KB edge buffer is allocated ONLY when the probe is armed, so a
    // normal build carries a pointer rather than the array.  Both sidetone
    // sinks hold a probe by value, so the disabled case is now free.  The
    // constructor is not real-time — scan()/record() never allocate. (#5200)
    CwSidetoneEdgeProbe()
        : m_enabled(qEnvironmentVariable("AETHER_CW_EDGE_PROBE") == QLatin1String("1"))
    {
        if (m_enabled)
            m_edges = std::make_unique<std::array<Edge, kMaxEdges>>();
    }

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
        if (!m_enabled) { return; }

        // An armed probe that saw nothing still has to reset — and still has
        // to SAY so. Returning early on m_count == 0 skipped the reset below,
        // so a stream that recorded no edges leaked its whole sample count
        // into the next stream: after 10 s of silence the next stream's first
        // edge reported samplePos 480001 instead of ~0, and the positions this
        // class advertises as "the stream's own clock" were off by that much.
        // The silent return also made an under-threshold sidetone (see
        // kThreshold) indistinguishable from a probe that was never armed.
        if (m_count == 0) {
            qCInfo(lcAudio).nospace()
                << "EDGEPROBE " << tag << " rate= " << sampleRateHz
                << " edges= 0 — armed, but no sample exceeded the "
                << kThreshold << " threshold (sidetone level too low?)";
            reset();
            return;
        }

        qCInfo(lcAudio) << "EDGEPROBE" << tag << "rate=" << sampleRateHz
                        << "edges=" << m_count
                        << (m_count == kMaxEdges ? "(TRUNCATED)" : "");
        for (int i = 0; i < m_count; ++i) {
            qCInfo(lcAudio).nospace()
                << "EDGEPROBE " << tag << ' '
                << (*m_edges)[i].samplePos << ((*m_edges)[i].rising ? " R" : " F");
        }
        reset();
    }

    struct Edge { qint64 samplePos; bool rising; };

    // Edges captured since the last dump(), and the edge at `i` (0 <= i <
    // edgeCount()).  Exposed so cw_sidetone_edge_probe_test can assert what
    // the probe recorded and WHERE — the sample position is the whole product
    // of this class, and a position that silently carries a previous stream's
    // offset is the defect these accessors exist to pin.  Production code has
    // no reason to call either. (#5200)
    int  edgeCount() const { return m_count; }
    Edge edgeAt(int i) const { return (*m_edges)[i]; }

private:

    // Every exit from dump() goes through here — the reset used to live at
    // the bottom of dump() and was skipped by the empty-stream early return.
    void reset()
    {
        m_count = 0;
        m_samplePos = 0;
        m_quietStart = 0;
        m_tone = false;
        m_quietRun = 0;
    }

    void record(qint64 pos, bool rising)
    {
        if (m_edges && m_count < kMaxEdges)
            (*m_edges)[m_count++] = {pos, rising};
    }

    static constexpr float kThreshold = 0.02f;
    static constexpr int   kQuietRunSamples = 96;   // 2 ms @ 48 kHz
    static constexpr int   kMaxEdges = 8192;

    std::unique_ptr<std::array<Edge, kMaxEdges>> m_edges;
    int     m_count{0};
    qint64  m_samplePos{0};
    qint64  m_quietStart{0};
    int     m_quietRun{0};
    bool    m_tone{false};
    bool    m_enabled{false};
};

} // namespace AetherSDR
