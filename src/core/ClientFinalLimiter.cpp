#include "ClientFinalLimiter.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

float dbToLin(float db) noexcept
{
    return std::pow(10.0f, db / 20.0f);
}

float linToDb(float lin) noexcept
{
    return 20.0f * std::log10(std::max(lin, 1e-12f));
}

// Time-constant → one-pole coefficient.  `1 - exp(-1 / (fs * tau))`
// per sample.  Clamped to [0, 1] so a degenerate sampleRate doesn't
// produce a pathological coefficient.
float tauToCoeff(float tauSeconds, double sampleRate) noexcept
{
    if (tauSeconds <= 0.0f || sampleRate <= 0.0) return 1.0f;
    const float c = 1.0f - std::exp(-1.0f / (static_cast<float>(sampleRate)
                                             * tauSeconds));
    return std::clamp(c, 0.0f, 1.0f);
}

float finiteOr(float v, float fallback) noexcept
{
    return std::isfinite(v) ? v : fallback;
}

constexpr float kReleaseTauSec = 0.080f;    // 80 ms — moderately fast
                                            // release; voice-friendly
constexpr float kRmsTauSec     = 0.300f;    // 300 ms voice-friendly RMS
constexpr float kActivityTauSec = 3.0f;     // 3 s rolling limiter activity

// The look-ahead gain lands on the ceiling up to float rounding; only a
// sample past it by more than this counts as needing the safety clip.
constexpr float kSafetyTolerance = 1.0f + 1e-5f;

} // namespace

ClientFinalLimiter::ClientFinalLimiter() = default;

void ClientFinalLimiter::prepare(double sampleRate)
{
    m_sampleRate.store(sampleRate, std::memory_order_relaxed);
    m_cached.releaseCoeff = tauToCoeff(kReleaseTauSec, sampleRate);
    m_cached.rmsCoeff     = tauToCoeff(kRmsTauSec,     sampleRate);
    // 25 Hz DC-block: y[n] = x[n] - x[n-1] + a·y[n-1] where
    // a ≈ 1 - 2π·fc / fs.  Clamp to (0, 1) so we don't go unstable
    // at degenerate sample rates.
    constexpr float kDcCornerHz = 25.0f;
    const float a = 1.0f
        - 2.0f * 3.14159265358979323846f * kDcCornerHz
              / static_cast<float>(std::max(1.0, sampleRate));
    m_cached.dcCoeff = std::clamp(a, 0.0f, 0.9999f);
    // The look-ahead in frames depends on the rate: recache it.
    m_cachedVersion = static_cast<uint64_t>(-1);
    recacheIfDirty();
    reset();
}

void ClientFinalLimiter::setEnabled(bool on) noexcept
{
    m_atomics.enabled.store(on, std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}

bool ClientFinalLimiter::isEnabled() const noexcept
{
    return m_atomics.enabled.load(std::memory_order_relaxed);
}

void ClientFinalLimiter::setCeilingDb(float db) noexcept
{
    m_atomics.ceilingDb.store(
        std::clamp(finiteOr(db, kDefaultCeilingDb), -12.0f, 0.0f),
        std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}

float ClientFinalLimiter::ceilingDb() const noexcept
{
    return m_atomics.ceilingDb.load(std::memory_order_relaxed);
}

void ClientFinalLimiter::setLookaheadMs(float ms) noexcept
{
    m_atomics.lookaheadMs.store(
        std::clamp(finiteOr(ms, kDefaultLookaheadMs), 0.0f, kMaxLookaheadMs),
        std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}

float ClientFinalLimiter::lookaheadMs() const noexcept
{
    return m_atomics.lookaheadMs.load(std::memory_order_relaxed);
}

int ClientFinalLimiter::framesForMs(float ms, double rate) const noexcept
{
    const long frames = std::lround(static_cast<double>(ms) * rate / 1000.0);
    return static_cast<int>(std::clamp<long>(frames, 0, kMaxLookaheadFrames));
}

int ClientFinalLimiter::latencyFrames() const noexcept
{
    if (!isEnabled()) return 0;
    return framesForMs(lookaheadMs(), sampleRate());
}

void ClientFinalLimiter::setOutputTrimDb(float db) noexcept
{
    m_atomics.outputTrimDb.store(std::clamp(finiteOr(db, 0.0f), -12.0f, 12.0f),
                                 std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}

float ClientFinalLimiter::outputTrimDb() const noexcept
{
    return m_atomics.outputTrimDb.load(std::memory_order_relaxed);
}

void ClientFinalLimiter::setDcBlockEnabled(bool on) noexcept
{
    m_atomics.dcBlock.store(on, std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}

bool ClientFinalLimiter::dcBlockEnabled() const noexcept
{
    return m_atomics.dcBlock.load(std::memory_order_relaxed);
}

float ClientFinalLimiter::inputPeakDb() const noexcept
{
    return m_meters.inputPeakDb.load(std::memory_order_relaxed);
}

float ClientFinalLimiter::outputPeakDb() const noexcept
{
    return m_meters.outputPeakDb.load(std::memory_order_relaxed);
}

float ClientFinalLimiter::outputRmsDb() const noexcept
{
    return m_meters.outputRmsDb.load(std::memory_order_relaxed);
}

float ClientFinalLimiter::gainReductionDb() const noexcept
{
    return m_meters.gainReductionDb.load(std::memory_order_relaxed);
}

bool ClientFinalLimiter::active() const noexcept
{
    return m_meters.active.load(std::memory_order_relaxed);
}

quint64 ClientFinalLimiter::clipPreLimiterCount() const noexcept
{
    return m_meters.clipPreLimiterCount.load(std::memory_order_relaxed);
}

float ClientFinalLimiter::limiterActivityPct() const noexcept
{
    return m_meters.limiterActivityPct.load(std::memory_order_relaxed);
}

quint64 ClientFinalLimiter::safetyClipCount() const noexcept
{
    return m_meters.safetyClipCount.load(std::memory_order_relaxed);
}

void ClientFinalLimiter::recacheIfDirty() noexcept
{
    const uint64_t v = m_atomics.version.load(std::memory_order_acquire);
    if (v == m_cachedVersion) return;
    m_cached.enabled    = m_atomics.enabled.load(std::memory_order_relaxed);
    m_cached.ceilingLin = dbToLin(
        m_atomics.ceilingDb.load(std::memory_order_relaxed));
    m_cached.trimLin    = dbToLin(
        m_atomics.outputTrimDb.load(std::memory_order_relaxed));
    m_cached.dcBlock    = m_atomics.dcBlock.load(std::memory_order_relaxed);
    m_cached.lookaheadFrames = framesForMs(
        m_atomics.lookaheadMs.load(std::memory_order_relaxed), sampleRate());
    m_cachedVersion = v;
}

void ClientFinalLimiter::resetGainPath(int window, float gain) noexcept
{
    m_window = std::clamp(window, 1, kRing);
    m_held   = gain;
    m_qHead  = 0;
    m_qSize  = 0;
    std::fill(m_avgRing.begin(), m_avgRing.end(), gain);
    m_avgPos = 0;
    m_avgSum = static_cast<double>(gain) * m_window;
}

void ClientFinalLimiter::reset() noexcept
{
    m_msAcc      = 0.0f;
    m_dcXprevL   = 0.0f;
    m_dcYprevL   = 0.0f;
    m_dcXprevR   = 0.0f;
    m_dcYprevR   = 0.0f;
    m_activityAcc = 0.0f;
    m_delayL.fill(0.0f);
    m_delayR.fill(0.0f);
    m_sampleIndex = 0;
    m_tap         = m_cached.enabled ? m_cached.lookaheadFrames : 0;
    m_fadeFromTap = m_tap;
    m_fadeLeft    = 0;
    resetGainPath(m_tap + 1, 1.0f);
}

void ClientFinalLimiter::process(float* interleaved, int frames, int channels) noexcept
{
    if (!interleaved || frames <= 0 || channels < 1 || channels > 2) return;
    recacheIfDirty();

    const float ceilingLin = std::max(m_cached.ceilingLin, 1e-6f);
    const float trimLin    = m_cached.trimLin;
    const float limRelease = m_cached.releaseCoeff;
    const float rmsCoeff   = m_cached.rmsCoeff;
    const float dcCoeff    = m_cached.dcCoeff;
    const bool  enabled    = m_cached.enabled;
    const bool  dcBlock    = m_cached.dcBlock;

    // A new look-ahead, or the limiter switching on or off, moves the delay
    // tap. Fade from the old tap to the new one instead of jumping, and
    // restart the gain path for the new window. While the fade runs, audio
    // the old tap delayed can reach the output under a gain computed for the
    // new window, so the safety clip may act. A change that arrives while a
    // fade is still running waits for it to finish: retargeting mid-fade
    // would drop the blend being output and jump to a different waveform.
    const int tap = enabled ? m_cached.lookaheadFrames : 0;
    if (tap != m_tap && m_fadeLeft == 0) {
        const bool unprepared = m_tap < 0;
        m_fadeFromTap = unprepared ? tap : m_tap;
        m_fadeLeft    = unprepared ? 0 : kCrossfadeFrames;
        m_tap         = tap;
        resetGainPath(tap + 1, enabled ? std::min(m_held, 1.0f) : 1.0f);
    }

    const auto slot = [](int64_t index) {
        return static_cast<std::size_t>(((index % kRing) + kRing) % kRing);
    };

    float inPeakLin  = 0.0f;
    float outPeakLin = 0.0f;
    float worstGrDb  = 0.0f;
    quint64 firedCount  = 0;
    quint64 clipCount   = 0;
    quint64 safetyCount = 0;

    for (int f = 0; f < frames; ++f) {
        float l = interleaved[f * channels];
        float r = (channels == 2) ? interleaved[f * channels + 1] : l;

        // DC-block HPF (applied first so a DC-offset input doesn't
        // skew the limiter's peak detection).
        if (dcBlock) {
            const float yL = l - m_dcXprevL + dcCoeff * m_dcYprevL;
            m_dcXprevL = l;
            m_dcYprevL = yL;
            l = yL;
            const float yR = r - m_dcXprevR + dcCoeff * m_dcYprevR;
            m_dcXprevR = r;
            m_dcYprevR = yR;
            r = yR;
        }

        // Trim — applied PRE-limiter as a "drive into limiter" stage.
        // Positive trim makes the limiter work harder (more squash);
        // negative trim sits below the ceiling and the limiter passes
        // through.  Output is always ≤ ceiling regardless of trim,
        // because the limiter clamps on the trimmed signal below.
        l *= trimLin;
        r *= trimLin;

        const float maxIn = std::max(std::fabs(l), std::fabs(r));
        if (maxIn > inPeakLin) inPeakLin = maxIn;
        // Pre-limiter clip detection — anything within a hair of
        // 0 dBFS counts.  These are the events the OVR LED latches on.
        if (maxIn >= 0.999f) ++clipCount;

        const int64_t n = m_sampleIndex++;
        m_delayL[slot(n)] = l;
        m_delayR[slot(n)] = r;

        float gain = 1.0f;
        if (enabled) {
            // The gain this sample needs, released toward 1 from the last.
            const float need = maxIn > ceilingLin ? ceilingLin / maxIn : 1.0f;
            m_held = need < m_held ? need : m_held + (need - m_held) * limRelease;

            // Sliding minimum over the last m_window held gains. The expired
            // head goes first, so the ring never holds more than m_window.
            if (m_qSize > 0
                && m_qIndex[static_cast<std::size_t>(m_qHead)] <= n - m_window) {
                m_qHead = (m_qHead + 1) % m_window;
                --m_qSize;
            }
            while (m_qSize > 0) {
                const int back = (m_qHead + m_qSize - 1) % m_window;
                if (m_qGain[static_cast<std::size_t>(back)] < m_held) break;
                --m_qSize;
            }
            const int tail = (m_qHead + m_qSize) % m_window;
            m_qGain[static_cast<std::size_t>(tail)]  = m_held;
            m_qIndex[static_cast<std::size_t>(tail)] = n;
            ++m_qSize;
            const float windowMin = m_qGain[static_cast<std::size_t>(m_qHead)];

            // Its moving average over the same length: a ramp that lands on
            // the minimum by the time the sample that needed it leaves the
            // delay. Every minimum averaged for this output covers sample
            // n - L, so the gain never exceeds what that sample needs.
            m_avgSum += windowMin - m_avgRing[static_cast<std::size_t>(m_avgPos)];
            m_avgRing[static_cast<std::size_t>(m_avgPos)] = windowMin;
            m_avgPos = (m_avgPos + 1) % m_window;
            gain = std::min(1.0f, static_cast<float>(m_avgSum / m_window));
        }

        float outL = m_delayL[slot(n - m_tap)];
        float outR = m_delayR[slot(n - m_tap)];
        if (m_fadeLeft > 0) {
            const float w = static_cast<float>(m_fadeLeft) / kCrossfadeFrames;
            outL = w * m_delayL[slot(n - m_fadeFromTap)] + (1.0f - w) * outL;
            outR = w * m_delayR[slot(n - m_fadeFromTap)] + (1.0f - w) * outR;
            --m_fadeLeft;
        }
        l = outL * gain;
        r = outR * gain;

        if (enabled) {
            // Safety net: past float rounding, only a parameter change while
            // limiting (look-ahead, ceiling, on/off) can get here.
            const float peak = std::max(std::fabs(l), std::fabs(r));
            if (peak > ceilingLin) {
                if (peak > ceilingLin * kSafetyTolerance) ++safetyCount;
                l = std::clamp(l, -ceilingLin, ceilingLin);
                r = std::clamp(r, -ceilingLin, ceilingLin);
            }
            // Anything under ~0.005 dB of reduction is inaudible and
            // would otherwise latch the active indicator.
            if (gain < 0.9994f) {
                const float grDb = linToDb(gain);
                if (grDb < worstGrDb) worstGrDb = grDb;
                // Only count this sample as "limiter active" for the
                // trailing-window activity meter when the reduction
                // exceeds 0.5 dB.  Sub-audible release-tail samples
                // would otherwise inflate the % long after the actual
                // clamp has finished.  ~10^(-0.5/20) ≈ 0.9441.
                if (gain < 0.9441f) ++firedCount;
            }
        }

        interleaved[f * channels] = l;
        if (channels == 2) interleaved[f * channels + 1] = r;

        const float maxOut = std::max(std::fabs(l), std::fabs(r));
        if (maxOut > outPeakLin) outPeakLin = maxOut;

        // RMS smoother over (max-of-channels) energy so stereo voice
        // images read as one number on the panel.
        m_msAcc += rmsCoeff * (maxOut * maxOut - m_msAcc);
    }

    // Trailing-window limiter activity.  Per-block fired ratio is
    // smoothed via the same one-pole shape used elsewhere; tau ≈ 3 s.
    const float blockRatio = static_cast<float>(firedCount)
                           / static_cast<float>(std::max(1, frames));
    const float aCoeff = tauToCoeff(kActivityTauSec,
                                    sampleRate() / std::max(1, frames));
    m_activityAcc += aCoeff * (blockRatio - m_activityAcc);

    m_meters.inputPeakDb.store(linToDb(std::max(inPeakLin, 1e-6f)),
                               std::memory_order_relaxed);
    m_meters.outputPeakDb.store(linToDb(std::max(outPeakLin, 1e-6f)),
                                std::memory_order_relaxed);
    m_meters.outputRmsDb.store(linToDb(std::sqrt(std::max(m_msAcc, 1e-12f))),
                               std::memory_order_relaxed);
    m_meters.gainReductionDb.store(worstGrDb, std::memory_order_relaxed);
    // LIMIT lights only when the worst clamp this block was at least
    // 0.5 dB of GR — sub-audible reductions during the release-tail
    // shouldn't visually trigger the indicator.
    m_meters.active.store(worstGrDb < -0.5f, std::memory_order_relaxed);
    m_meters.limiterActivityPct.store(std::clamp(m_activityAcc, 0.0f, 1.0f),
                                      std::memory_order_relaxed);
    if (clipCount > 0) {
        m_meters.clipPreLimiterCount.fetch_add(
            clipCount, std::memory_order_relaxed);
    }
    if (safetyCount > 0) {
        m_meters.safetyClipCount.fetch_add(safetyCount, std::memory_order_relaxed);
    }
}

} // namespace AetherSDR
