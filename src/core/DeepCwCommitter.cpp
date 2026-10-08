#include "DeepCwCommitter.h"

#include <algorithm>

namespace AetherSDR {

namespace {
constexpr int    kRate     = DeepCwEngine::kModelSampleRate;
constexpr double kFrameSec = static_cast<double>(DeepCwEngine::kHopLength) / kRate;
constexpr int    kSnapBlankFrames = 3;
} // namespace

DeepCwCommitter::DeepCwCommitter(double holdSec, double leftContextSec, double hopSec)
    : m_hold(holdSec)
    , m_left(leftContextSec)
    , m_hop(hopSec)
    // Stay inside the model's trained 5-20 s window range.
    , m_window(std::min(DeepCwEngine::kMaxWindowSec,
                        std::max(15.0, holdSec + leftContextSec + hopSec)))
    , m_keep(std::min(m_window - hopSec, holdSec + leftContextSec))
{
}

void DeepCwCommitter::reset()
{
    m_buf.clear();
    m_bufStart = 0;
    m_lastDecodeEnd = 0;
    m_tCommit = -1.0;
    m_tLastChar = -1.0;
    m_lastOut = 0;
}

DeepCwCommitter::Result DeepCwCommitter::push(const float* audio3200, std::size_t n,
                                              const DeepCwEngine& eng)
{
    Result r;
    if (n > 0) m_buf.insert(m_buf.end(), audio3200, audio3200 + n);
    const std::size_t absEnd = m_bufStart + m_buf.size();
    if (m_buf.size() >= static_cast<std::size_t>(kRate * DeepCwEngine::kMinWindowSec)
        && absEnd >= m_lastDecodeEnd + static_cast<std::size_t>(m_hop * kRate)) {
        m_lastDecodeEnd = absEnd;
        r = decodeAndCommit(eng, false);
    }
    if (m_buf.size() >= static_cast<std::size_t>(m_window * kRate)) {
        const std::size_t drop = m_buf.size() - static_cast<std::size_t>(m_keep * kRate);
        m_buf.erase(m_buf.begin(), m_buf.begin() + static_cast<std::ptrdiff_t>(drop));
        m_bufStart += drop;
    }
    return r;
}

DeepCwCommitter::Result DeepCwCommitter::flush(const DeepCwEngine& eng)
{
    return decodeAndCommit(eng, true);
}

DeepCwCommitter::Result DeepCwCommitter::decodeAndCommit(const DeepCwEngine& eng, bool flushAll)
{
    Result r;
    int frames = 0;
    const std::vector<float> lp = eng.inferLogProbs(m_buf, &frames, &r.pitchHz);
    if (frames == 0) return r;
    r.decoded = true;

    std::vector<uint8_t> blank;
    const std::vector<DeepCwEngine::Emission> em = eng.greedyEmissions(lp.data(), frames, &blank);

    const double t0 = static_cast<double>(m_bufStart) / kRate;
    const double nowT = static_cast<double>(m_bufStart + m_buf.size()) / kRate;
    double cutoff = 1e18;
    if (!flushAll) {
        // Snap back to the latest frame <= (now - hold) that ends a blank run.
        int f = std::min(frames - 1, static_cast<int>((nowT - m_hold - t0) / kFrameSec));
        while (f >= kSnapBlankFrames
               && !(blank[f] && blank[f - 1] && blank[f - 2])) --f;
        cutoff = t0 + f * kFrameSec;
    }

    double confSum = 0.0;
    int confN = 0;
    for (const auto& e : em) {
        const double t = t0 + e.frame * kFrameSec;
        // Word spaces are emitted inside the silent gap the cutoff snaps into,
        // so a space may land just behind tCommit on the next decode: accept it
        // anywhere after the last committed letter.
        const double from = (e.ch == ' ') ? m_tLastChar : m_tCommit;
        if (t <= from || t > cutoff) continue;
        if (e.ch == ' ' && (m_lastOut == 0 || m_lastOut == ' ')) continue;
        r.text.push_back(e.ch);
        m_lastOut = e.ch;
        if (e.ch != ' ') {
            m_tLastChar = t;
            confSum += e.conf;
            ++confN;
        }
    }
    if (confN > 0) r.meanConf = static_cast<float>(confSum / confN);
    if (cutoff > m_tCommit) m_tCommit = cutoff;
    return r;
}

} // namespace AetherSDR
