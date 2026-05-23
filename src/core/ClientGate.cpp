#include "ClientGate.h"

#include <algorithm>
#include <cmath>

namespace MasterSDR {

namespace {

float dbToLin(float db) noexcept
{
    return std::pow(10.0f, db * 0.05f);
}

float linToDb(float lin) noexcept
{
    return (lin > 1e-9f) ? 20.0f * std::log10(lin) : -180.0f;
}

float coeffFromMs(float ms, double sampleRate) noexcept
{
    if (ms <= 0.0f) return 1.0f;
    const float tau = ms * 0.001f;
    return 1.0f - std::exp(-1.0f / static_cast<float>(sampleRate * tau));
}

} // namespace

ClientGate::ClientGate() = default;

void ClientGate::prepare(double sampleRate)
{
    m_sampleRate    = sampleRate;
    m_envLin        = 0.0f;
    m_currentGainDb = 0.0f;
    m_holdCountdown = 0;
    m_isOpen        = false;

    // Allocate the lookahead delay line once at prepare() so process()
    // never allocates.  Size it for kMaxLookaheadMs + 1 frame of slack.
    m_delayCap = static_cast<int>(kMaxLookaheadMs * 0.001f
                                  * static_cast<float>(sampleRate) + 0.5f) + 1;
    m_delay.assign(m_delayCap * 2, 0.0f);   // stereo interleaved
    m_delayWrite = 0;

    m_atomics.version.fetch_add(1, std::memory_order_release);
    recacheIfDirty();
}

void ClientGate::setEnabled(bool on) noexcept
{
    m_atomics.enabled.store(on, std::memory_order_release);
}
bool ClientGate::isEnabled() const noexcept
{
    return m_atomics.enabled.load(std::memory_order_acquire);
}

void ClientGate::setMode(Mode m) noexcept
{
    m_atomics.mode.store(static_cast<uint8_t>(m), std::memory_order_relaxed);
    // Preset pairs — only ratio + floor change; threshold / attack /
    // release / hold / return / lookahead are left alone so user
    // fine-tuning survives a mode toggle.
    if (m == Mode::Expander) {
        m_atomics.ratio.store(2.0f,    std::memory_order_relaxed);
        m_atomics.floorDb.store(-15.0f, std::memory_order_relaxed);
    } else {
        m_atomics.ratio.store(10.0f,   std::memory_order_relaxed);
        m_atomics.floorDb.store(-40.0f, std::memory_order_relaxed);
    }
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
ClientGate::Mode ClientGate::mode() const noexcept
{
    return static_cast<Mode>(
        m_atomics.mode.load(std::memory_order_relaxed));
}

void ClientGate::setThresholdDb(float db) noexcept
{
    m_atomics.thresholdDb.store(std::clamp(db, -80.0f, 0.0f),
                                std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientGate::thresholdDb() const noexcept
{ return m_atomics.thresholdDb.load(std::memory_order_relaxed); }

void ClientGate::setRatio(float ratio) noexcept
{
    m_atomics.ratio.store(std::clamp(ratio, 1.0f, 10.0f),
                          std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientGate::ratio() const noexcept
{ return m_atomics.ratio.load(std::memory_order_relaxed); }

void ClientGate::setAttackMs(float ms) noexcept
{
    m_atomics.attackMs.store(std::clamp(ms, 0.1f, 100.0f),
                             std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientGate::attackMs() const noexcept
{ return m_atomics.attackMs.load(std::memory_order_relaxed); }

void ClientGate::setReleaseMs(float ms) noexcept
{
    m_atomics.releaseMs.store(std::clamp(ms, 5.0f, 2000.0f),
                              std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientGate::releaseMs() const noexcept
{ return m_atomics.releaseMs.load(std::memory_order_relaxed); }

void ClientGate::setHoldMs(float ms) noexcept
{
    m_atomics.holdMs.store(std::clamp(ms, 0.0f, 500.0f),
                           std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientGate::holdMs() const noexcept
{ return m_atomics.holdMs.load(std::memory_order_relaxed); }

void ClientGate::setFloorDb(float db) noexcept
{
    m_atomics.floorDb.store(std::clamp(db, -80.0f, 0.0f),
                            std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientGate::floorDb() const noexcept
{ return m_atomics.floorDb.load(std::memory_order_relaxed); }

void ClientGate::setReturnDb(float db) noexcept
{
    m_atomics.returnDb.store(std::clamp(db, 0.0f, 20.0f),
                             std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientGate::returnDb() const noexcept
{ return m_atomics.returnDb.load(std::memory_order_relaxed); }

void ClientGate::setLookaheadMs(float ms) noexcept
{
    m_atomics.lookaheadMs.store(
        std::clamp(ms, 0.0f, static_cast<float>(kMaxLookaheadMs)),
        std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}
float ClientGate::lookaheadMs() const noexcept
{ return m_atomics.lookaheadMs.load(std::memory_order_relaxed); }

void ClientGate::reset() noexcept
{
    m_envLin        = 0.0f;
    m_currentGainDb = 0.0f;
    m_holdCountdown = 0;
    m_isOpen        = false;
    std::fill(m_delay.begin(), m_delay.end(), 0.0f);
    m_delayWrite    = 0;
}

float ClientGate::inputPeakDb() const noexcept
{ return m_meters.inputPeakDb.load(std::memory_order_relaxed); }
float ClientGate::outputPeakDb() const noexcept
{ return m_meters.outputPeakDb.load(std::memory_order_relaxed); }
float ClientGate::gainReductionDb() const noexcept
{ return m_meters.gainReductionDb.load(std::memory_order_relaxed); }
bool  ClientGate::gateOpen() const noexcept
{ return m_meters.gateOpen.load(std::memory_order_relaxed); }

void ClientGate::recacheIfDirty() noexcept
{
    const uint64_t v = m_atomics.version.load(std::memory_order_acquire);
    if (v == m_lastVersion) return;
    m_lastVersion = v;

    m_cached.thresholdDb = m_atomics.thresholdDb.load(std::memory_order_relaxed);
    const float r        = std::max(1.0f,
                                    m_atomics.ratio.load(std::memory_order_relaxed));
    m_cached.slope       = r - 1.0f;
    m_cached.attackCoeff = coeffFromMs(
        m_atomics.attackMs.load(std::memory_order_relaxed), m_sampleRate);
    m_cached.releaseCoeff = coeffFromMs(
        m_atomics.releaseMs.load(std::memory_order_relaxed), m_sampleRate);
    // Fixed fast envelope-detector release — decouples threshold
    // detection from the user's gain-smoother release.  Without this
    // a long release_ms would also stretch how long the envelope
    // stays "above threshold" after a loud burst, pushing the hold
    // window far past where the user expects it.
    m_cached.envReleaseCoeff = coeffFromMs(10.0f, m_sampleRate);
    m_cached.holdSamples = static_cast<int>(
        m_atomics.holdMs.load(std::memory_order_relaxed) * 0.001f
        * static_cast<float>(m_sampleRate) + 0.5f);
    m_cached.floorDb     = m_atomics.floorDb.load(std::memory_order_relaxed);
    m_cached.closeThresholdDb = m_cached.thresholdDb
        - m_atomics.returnDb.load(std::memory_order_relaxed);
    // Clamp lookahead to the delay-line capacity minus one (so write
    // and read indices can never alias on the same frame).
    const int laSamples = static_cast<int>(
        m_atomics.lookaheadMs.load(std::memory_order_relaxed) * 0.001f
        * static_cast<float>(m_sampleRate) + 0.5f);
    m_cached.lookaheadSamples =
        std::clamp(laSamples, 0, std::max(0, m_delayCap - 1));
}

float ClientGate::staticCurveGainDb(float envDb) const noexcept
{
    // Downward expander curve — attenuates below threshold, unity above.
    //   gain_dB = clamp(-(T - env) * (ratio - 1), floor, 0)
    // At env = T, gain = 0 (unity).  For env < T the attenuation grows
    // linearly with shortfall.  floor is the attenuation limit (e.g.
    // -40 dB for a hard gate), clamping the curve so the gate does not
    // invert into infinite attenuation at very quiet envelope levels.
    const float T         = m_cached.thresholdDb;
    const float shortfall = T - envDb;
    if (shortfall <= 0.0f) return 0.0f;          // above threshold: unity
    const float gainDb = -shortfall * m_cached.slope;
    return std::max(gainDb, m_cached.floorDb);   // clamp to floor
}

void ClientGate::process(float* interleaved, int frames, int channels) noexcept
{
    if (frames <= 0) return;
    if (channels != 1 && channels != 2) return;

    recacheIfDirty();
    const bool enabled = m_atomics.enabled.load(std::memory_order_acquire);

    float inPeakLin  = 0.0f;
    float outPeakLin = 0.0f;
    float worstGrDb  = 0.0f;   // most negative (largest attenuation)
    bool  openAny    = false;  // true if gate was above threshold any sample

    const float openT           = m_cached.thresholdDb;
    const float closeT          = m_cached.closeThresholdDb;
    const float attackCoeff     = m_cached.attackCoeff;
    const float releaseCoeff    = m_cached.releaseCoeff;
    const float envReleaseCoeff = m_cached.envReleaseCoeff;
    const int   holdSamples     = m_cached.holdSamples;
    const int   laSamples       = m_cached.lookaheadSamples;

    for (int f = 0; f < frames; ++f) {
        // Read the new input (applied to the detector) and write it
        // to the delay line.  The delayed sample that we multiply by
        // gain is read BEFORE the write — so with lookahead = N the
        // gain we compute from input[n] is applied to input[n-N].
        const float inL = interleaved[f * channels];
        const float inR = (channels == 2) ? interleaved[f * channels + 1] : inL;

        const float inAbs = std::max(std::fabs(inL), std::fabs(inR));
        if (inAbs > inPeakLin) inPeakLin = inAbs;

        // Delayed sample pair — what we'll actually multiply by gain.
        float outL = inL, outR = inR;
        if (laSamples > 0 && m_delayCap > 0) {
            int readIdx = m_delayWrite - laSamples;
            if (readIdx < 0) readIdx += m_delayCap;
            outL = m_delay[readIdx * 2];
            outR = m_delay[readIdx * 2 + 1];
            m_delay[m_delayWrite * 2]     = inL;
            m_delay[m_delayWrite * 2 + 1] = inR;
            m_delayWrite = (m_delayWrite + 1) % m_delayCap;
        }

        float gainLin = 1.0f;
        if (enabled) {
            // Fast peak-detector envelope — used purely for
            // threshold detection.  User ballistics apply to the
            // gain smoother below.  Instant attack, fixed 10 ms
            // release so the detector tracks peaks accurately.
            const float envAlpha = (inAbs > m_envLin)
                ? 1.0f
                : envReleaseCoeff;
            m_envLin += envAlpha * (inAbs - m_envLin);
            const float envDb = linToDb(std::max(m_envLin, 1e-6f));

            // Schmitt-trigger threshold detection:
            //   - closed → open when envDb ≥ thresholdDb
            //   - open → closed when envDb < thresholdDb - returnDb
            // Curve target tracks m_isOpen so the gain smoother
            // sees a clean step (no flicker when envelope hovers
            // near the threshold).
            if (m_isOpen) {
                if (envDb < closeT) m_isOpen = false;
            } else {
                if (envDb >= openT) m_isOpen = true;
            }
            if (m_isOpen) openAny = true;

            const float targetGainDb = m_isOpen ? 0.0f
                                                : staticCurveGainDb(envDb);

            // Gain state machine:
            //   - opening (less attenuation wanted): slide toward
            //     target using attack coefficient
            //   - closing (more attenuation wanted): freeze gain
            //     for holdSamples, then slide toward target using
            //     release coefficient
            if (targetGainDb >= m_currentGainDb) {
                m_currentGainDb += attackCoeff
                    * (targetGainDb - m_currentGainDb);
                m_holdCountdown = holdSamples;
            } else {
                if (m_holdCountdown > 0) {
                    --m_holdCountdown;
                } else {
                    m_currentGainDb += releaseCoeff
                        * (targetGainDb - m_currentGainDb);
                }
            }

            if (m_currentGainDb < worstGrDb) worstGrDb = m_currentGainDb;
            gainLin = dbToLin(m_currentGainDb);
        }

        outL *= gainLin;
        outR *= gainLin;

        interleaved[f * channels] = outL;
        if (channels == 2) interleaved[f * channels + 1] = outR;

        const float outAbs = std::max(std::fabs(outL), std::fabs(outR));
        if (outAbs > outPeakLin) outPeakLin = outAbs;
    }

    m_meters.inputPeakDb.store(linToDb(std::max(inPeakLin, 1e-6f)),
                               std::memory_order_relaxed);
    m_meters.outputPeakDb.store(linToDb(std::max(outPeakLin, 1e-6f)),
                                std::memory_order_relaxed);
    m_meters.gainReductionDb.store(worstGrDb, std::memory_order_relaxed);
    m_meters.gateOpen.store(openAny, std::memory_order_relaxed);
}

} // namespace MasterSDR
