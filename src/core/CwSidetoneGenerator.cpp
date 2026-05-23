#include "CwSidetoneGenerator.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QVarLengthArray>

namespace MasterSDR {

namespace {

constexpr double kPi = 3.141592653589793238462643383279;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kPiOver2 = kPi / 2.0;

// Raised-cosine envelope: env(i) = 0.5 * (1 - cos(pi * i / N)) for ramp-up,
// reversed for ramp-down.  Smooth attack/release prevents the harsh "click"
// that hard-keying produces, without audibly slurring the leading edge.
inline float raisedCosineUp(int i, int N) noexcept
{
    if (N <= 0) return 1.0f;
    const double t = static_cast<double>(i) / static_cast<double>(N);
    return static_cast<float>(0.5 * (1.0 - std::cos(kPi * t)));
}

inline float raisedCosineDown(int i, int N) noexcept
{
    return raisedCosineUp(N - i, N);
}

inline float clampf(float v, float lo, float hi) noexcept
{
    return std::clamp(v, lo, hi);
}

} // namespace

CwSidetoneGenerator::CwSidetoneGenerator(int sampleRateHz)
    : m_sampleRateHz(sampleRateHz > 0 ? sampleRateHz : 48000)
{
    m_rampLength = std::max(1, static_cast<int>(m_shapingMs.load() *
                                                m_sampleRateHz / 1000.0f));
}

void CwSidetoneGenerator::setEnabled(bool on) noexcept
{
    m_enabled.store(on, std::memory_order_relaxed);
}

void CwSidetoneGenerator::setPitchHz(float hz) noexcept
{
    m_pitchHz.store(clampf(hz, 100.0f, 4000.0f), std::memory_order_relaxed);
}

void CwSidetoneGenerator::setVolume(float v) noexcept
{
    m_volume.store(clampf(v, 0.0f, 1.0f), std::memory_order_relaxed);
}

void CwSidetoneGenerator::setShapingMs(float ms) noexcept
{
    m_shapingMs.store(clampf(ms, 0.0f, 50.0f), std::memory_order_relaxed);
}

void CwSidetoneGenerator::setPan(float p) noexcept
{
    m_pan.store(clampf(p, 0.0f, 1.0f), std::memory_order_relaxed);
}

void CwSidetoneGenerator::setKeyDown(bool down) noexcept
{
    m_keyDown.store(down, std::memory_order_relaxed);
}

void CwSidetoneGenerator::reset() noexcept
{
    m_state = State::Idle;
    m_rampSample = 0;
    m_phase = 0.0;
}

void CwSidetoneGenerator::setSampleRateHz(int hz) noexcept
{
    m_sampleRateHz = (hz > 0) ? hz : 48000;
    m_rampLength = std::max(1, static_cast<int>(m_shapingMs.load() *
                                                m_sampleRateHz / 1000.0f));
    reset();
}

bool CwSidetoneGenerator::process(float* out, int frames) noexcept
{
    const bool tapSet = static_cast<bool>(m_sampleTap);

    if (!m_enabled.load(std::memory_order_relaxed)) {
        // Disabled — bring state back to idle on next block so a flip-on
        // mid-keying starts cleanly from silence.
        if (m_state != State::Idle)
            reset();
        if (tapSet) {
            // Mirror silence to the TX-decode tap so the downstream
            // decoder's timeline doesn't jump forward across the gap.
            QVarLengthArray<float, 1024> silence(frames);
            std::memset(silence.data(), 0, frames * sizeof(float));
            m_sampleTap(silence.data(), frames, m_sampleRateHz);
        }
        return false;
    }

    const bool keyDown = m_keyDown.load(std::memory_order_relaxed);
    const float pitch  = m_pitchHz.load(std::memory_order_relaxed);
    const float vol    = m_volume.load(std::memory_order_relaxed);
    const float shapingMs = m_shapingMs.load(std::memory_order_relaxed);

    // Recompute ramp length if shaping changed.  Cheap; bounds checked.
    const int newRampLen =
        std::max(1, static_cast<int>(shapingMs * m_sampleRateHz / 1000.0f));
    if (newRampLen != m_rampLength) {
        // Rescale current rampSample to the new length so a live shaping
        // change mid-ramp doesn't snap the envelope.
        if (m_rampLength > 0)
            m_rampSample = (m_rampSample * newRampLen) / m_rampLength;
        m_rampLength = newRampLen;
    }

    // Edge transitions: drive the state machine off the current key state.
    switch (m_state) {
    case State::Idle:
        if (keyDown) {
            m_state = State::RampUp;
            m_rampSample = 0;
        }
        break;
    case State::RampUp:
        if (!keyDown) {
            m_state = State::RampDown;
            // Continue from the current envelope position so a quick
            // dot doesn't audibly click — start ramp-down from where
            // ramp-up left off, scaled to the same envelope value.
            m_rampSample = m_rampLength - m_rampSample;
        }
        break;
    case State::Sustain:
        if (!keyDown) {
            m_state = State::RampDown;
            m_rampSample = 0;
        }
        break;
    case State::RampDown:
        if (keyDown) {
            // Mirror image of RampUp→RampDown: re-enter from current
            // envelope position.
            m_state = State::RampUp;
            m_rampSample = m_rampLength - m_rampSample;
        }
        break;
    }

    if (m_state == State::Idle && !keyDown) {
        if (tapSet) {
            QVarLengthArray<float, 1024> silence(frames);
            std::memset(silence.data(), 0, frames * sizeof(float));
            m_sampleTap(silence.data(), frames, m_sampleRateHz);
        }
        return false;
    }

    const double phaseInc = kTwoPi * pitch / m_sampleRateHz;
    // Constant-power pan: equal perceived loudness across the L↔R sweep.
    const float pan = m_pan.load(std::memory_order_relaxed);
    const float gainL = std::cos(pan * static_cast<float>(kPiOver2));
    const float gainR = std::sin(pan * static_cast<float>(kPiOver2));
    bool wroteAny = false;

    // Mono mirror buffer for the TX-decode tap.  Stack-allocated up to
    // ~4 KB; falls back to heap for unusually large blocks.
    QVarLengthArray<float, 1024> tapBuf;
    if (tapSet) tapBuf.resize(frames);

    for (int i = 0; i < frames; ++i) {
        float env = 0.0f;
        switch (m_state) {
        case State::Idle:
            env = 0.0f;
            break;
        case State::RampUp:
            env = raisedCosineUp(m_rampSample, m_rampLength);
            if (++m_rampSample >= m_rampLength) {
                m_state = State::Sustain;
                m_rampSample = 0;
            }
            break;
        case State::Sustain:
            env = 1.0f;
            break;
        case State::RampDown:
            env = raisedCosineDown(m_rampSample, m_rampLength);
            if (++m_rampSample >= m_rampLength) {
                m_state = State::Idle;
                m_rampSample = 0;
            }
            break;
        }

        const float sample = env * vol *
            static_cast<float>(std::sin(m_phase));
        out[2 * i + 0] += sample * gainL;  // L
        out[2 * i + 1] += sample * gainR;  // R
        if (tapSet) tapBuf[i] = sample;

        m_phase += phaseInc;
        if (m_phase >= kTwoPi) m_phase -= kTwoPi;

        if (env > 0.0f) wroteAny = true;
    }

    if (tapSet) m_sampleTap(tapBuf.data(), frames, m_sampleRateHz);

    m_lastPitchHz = pitch;
    return wroteAny;
}

} // namespace MasterSDR
