#include "DeepFistStream.h"
#include "DeepFistModel.h"
#include "DeepFistCtc.h"
#include <algorithm>
#include <cmath>
namespace AetherSDR {
bool DeepFistStream::Parameters::valid() const
{
    return tickSamples >= 320 && tickSamples <= 3200
        && std::isfinite(guardSeconds) && guardSeconds >= 0.4 && guardSeconds <= 3.0
        && std::isfinite(slowGuardSeconds) && slowGuardSeconds >= guardSeconds && slowGuardSeconds <= 3.0
        && slowBelowWpm >= 0 && slowBelowWpm <= 30
        && std::isfinite(activityThreshold) && activityThreshold >= 1.f && activityThreshold <= 100.f
        && (!normalizeActivity || (requireCompletedMark && recentActivitySamples == 0))
        && (recentActivitySamples == 0 || (requireCompletedMark
            && recentActivitySamples >= 4096 && recentActivitySamples <= 9600));
}
DeepFistStream::DeepFistStream(const Parameters& parameters)
    : m_parameters(parameters), m_failed(!parameters.valid())
{
}
bool DeepFistStream::hasCompletedMark(const float* samples, int count)
{
    // A rising edge alone is also a receiver/carrier startup. Require an
    // observed key-up after a plausible mark; never close the final run at EOF.
    // 5 ms energy bins suppress carrier-cycle ripple without erasing fast dits.
    if (!samples || count <= 0 || count > kWindow) { return false; }
    constexpr int kBinSamples = 16;
    std::vector<float> power;
    power.reserve(count / kBinSamples);
    float peak = 0.f;
    for (int i = 0; i + kBinSamples <= count; i += kBinSamples) {
        float sum = 0.f;
        for (int j = 0; j < kBinSamples; ++j) { sum += samples[i+j] * samples[i+j]; }
        power.push_back(sum);
        peak = std::max(peak, sum);
    }
    if (peak <= 1e-12f) { return false; }
    int run = 0;
    for (float value : power) {
        if (value > peak * 0.25f) { ++run; }
        else {
            if (run >= 2 && run <= 180) { return true; }
            run = 0;
        }
    }
    return false;
}
QString DeepFistStream::process(const float* samples, int count, lyra::dsp::DeepFistModel& model,
                                std::vector<Observation>* observations)
{
    if (m_failed) { return {}; }
    QString result;
    for (int i = 0; i < count; ++i) {
        m_ring[m_samples % kWindow] = samples[i];
        ++m_samples;
        if (m_samples % m_parameters.tickSamples != 0 || m_samples < 3200) { continue; }
        const int start = static_cast<int>(m_samples % kWindow);
        std::copy(m_ring.begin() + start, m_ring.end(), m_window.begin());
        std::copy(m_ring.begin(), m_ring.begin() + start, m_window.end() - start);
        const double end = m_samples / 3200.0;
        // Slow characters need more right context. A longer guard at every speed
        // regressed the comparison set; keep it conditional on the keying estimate.
        // Experimental thresholds, not a calibrated WPM reading for the UI.
        const int wpm = model.keyingWpm(m_window.data(), kWindow);
        const double settled = end - ((wpm > 0 && wpm < m_parameters.slowBelowWpm)
            ? m_parameters.slowGuardSeconds : m_parameters.guardSeconds);
        // Normalize only the gate measurement. The fixed floor in keyingRatio
        // otherwise suppresses quiet clean signals that inference can recognize.
        // Keep the trained model's input and conditioning unchanged.
        std::vector<float> activity;
        if (m_parameters.normalizeActivity) {
            activity = m_window;
            float peak = 0.f;
            for (float value : activity) { peak = std::max(peak, std::abs(value)); }
            if (peak > 1e-9f && std::isfinite(peak)) {
                for (float& value : activity) { value = (value / peak) * 0.2f; }
            }
        }
        const float ratio = model.keyingRatio(m_parameters.normalizeActivity
            ? activity.data() : m_window.data(), kWindow);
        const float recentRatio = m_parameters.recentActivitySamples == 0 ? 0.f
            : model.keyingRatio(m_window.data() + kWindow - m_parameters.recentActivitySamples,
                m_parameters.recentActivitySamples);
        const bool completedMark = !m_parameters.requireCompletedMark && !observations
            ? true : hasCompletedMark(m_window.data(), kWindow);
        std::vector<DeepFistCommitter::Token> tokens;
        Observation observation{end, static_cast<int>(std::min<quint64>(m_samples, kWindow)),
            ratio, 0.f, wpm, settled, completedMark};
        observation.committedBefore = m_committer.committed();
        observation.recentRatio = recentRatio;
        const bool gated = std::max(ratio, recentRatio) < m_parameters.activityThreshold
            || (m_parameters.requireCompletedMark && !completedMark);
        observation.gated = gated;
        if (observations) {
            observation.realRatio = model.keyingRatio(m_window.data() + kWindow - observation.realSamples,
                observation.realSamples);
        }
        // Developer traces also capture rejected windows. Shadow inference never
        // publishes text; the normal app still skips all gated inference.
        if (!gated || observations) {
            int frames = 0, classes = 0;
            if (!model.infer(m_window.data(), kWindow, m_logits, frames, classes)
                || frames <= 0 || classes != 48) {
                m_failed = true;
                return {};
            }
            const lyra::dsp::CtcFrames decoded = lyra::dsp::greedyCtcFrames(m_logits.data(), frames, classes);
            // greedyCtcFrames() does not apply the token-table bound that
            // greedyCtcDecode() does, and it is vendored, so the check belongs
            // here: .at() on an out-of-range id throws on the decode worker,
            // which has no handler above it.
            const std::size_t tokenCount = model.tokens().size();
            for (std::size_t j = 0; j < decoded.ids.size(); ++j) {
                if (decoded.ids[j] < 0 || static_cast<std::size_t>(decoded.ids[j]) >= tokenCount) {
                    continue;
                }
                const double time = end - 6.0 + decoded.frames[j] * 6.0 / frames;
                const QString token = QString::fromStdString(model.tokens()[decoded.ids[j]]);
                tokens.push_back({decoded.ids[j], time, token});
                if (observations) {
                    const QString decision = gated ? QStringLiteral("activity_gate")
                        : time <= m_committer.committed() ? QStringLiteral("past_boundary")
                        : time > settled ? QStringLiteral("pending") : QStringLiteral("commit");
                    observation.tokens.push_back({decoded.ids[j], decoded.frames[j], time, token, decision});
                }
            }
        }
        const QString publication = m_committer.process(tokens, gated, end, settled, m_parameters.carryPending);
        result += publication;
        if (observations) {
            observation.publication = publication;
            observation.committedAfter = m_committer.committed();
            observations->push_back(std::move(observation));
        }
    }
    return result;
}
}
