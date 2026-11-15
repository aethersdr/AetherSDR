#pragma once

// Sliding-window CTC decode with a time-anchored commit point. Text stays
// provisional until holdSec behind the live edge, then commits at a blank run.
// Trimming keeps holdSec + leftContextSec so letters can span window edges.
// Qt-free so the offline replay tool uses the same streaming path.

#include "DeepCwEngine.h"

#include <cstddef>
#include <string>
#include <vector>

namespace AetherSDR {

class DeepCwCommitter {
public:
    explicit DeepCwCommitter(double holdSec = 5.0, double leftContextSec = 3.0,
                             double hopSec = 2.0);

    struct Result {
        bool decoded{false};    // a model decode ran on this push
        std::string text;       // newly committed characters (may be empty)
        float meanConf{1.0f};   // mean posterior of the committed characters
        float pitchHz{0.0f};    // dominant tone of the decoded window
    };

    // Append model-rate (3200 Hz) mono audio; decodes when a hop has elapsed.
    Result push(const float* audio3200, std::size_t n, const DeepCwEngine& eng);

    // Commit everything still provisional (offline end-of-file use).
    Result flush(const DeepCwEngine& eng);

    void reset();

    double holdSec() const { return m_hold; }
    double windowSec() const { return m_window; }

private:
    Result decodeAndCommit(const DeepCwEngine& eng, bool flushAll);

    double m_hold, m_left, m_hop, m_window, m_keep;
    std::vector<float> m_buf;
    std::size_t m_bufStart{0};        // absolute sample index of m_buf[0]
    std::size_t m_lastDecodeEnd{0};
    double m_tCommit{-1.0};           // committed up to this time (s)
    double m_tLastChar{-1.0};         // emission time of the last committed letter
    char m_lastOut{0};
};

} // namespace AetherSDR
