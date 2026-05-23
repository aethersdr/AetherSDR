#include "ClientQuindarTone.h"

#include <algorithm>
#include <cmath>

namespace MasterSDR {

namespace {

constexpr float kTwoPi      = 6.28318530717958647692f;
constexpr float kEnvRampSec = 0.005f;       // 5 ms cos² ramp at element edges

float dbToLin(float db) noexcept
{
    return std::pow(10.0f, db / 20.0f);
}

// Cos² envelope multiplier.  Returns 0 at frameInRamp = 0, 1 at
// frameInRamp = rampFrames.  Used at both the leading and trailing
// edges of every tone or Morse element to avoid key-clicks.
float cosSquaredRamp(int frameInRamp, int rampFrames) noexcept
{
    if (rampFrames <= 0) return 1.0f;
    if (frameInRamp <= 0) return 0.0f;
    if (frameInRamp >= rampFrames) return 1.0f;
    const float t = static_cast<float>(frameInRamp)
                    / static_cast<float>(rampFrames);
    // sin² goes 0 → 1 across [0, π/2]; equivalent shape to cos² fade-in.
    const float s = std::sin(t * 1.57079632679f);
    return s * s;
}

float envelopeAt(int phaseFrame, int totalFrames, int rampFrames) noexcept
{
    if (totalFrames <= 0) return 0.0f;
    if (phaseFrame < 0 || phaseFrame >= totalFrames) return 0.0f;
    const int distFromEnd = totalFrames - 1 - phaseFrame;
    const float rIn  = cosSquaredRamp(phaseFrame, rampFrames);
    const float rOut = cosSquaredRamp(distFromEnd, rampFrames);
    return std::min(rIn, rOut);
}

int rampFramesFor(double sampleRate) noexcept
{
    return std::max(1, static_cast<int>(kEnvRampSec * sampleRate));
}

} // namespace

ClientQuindarTone::ClientQuindarTone()
{
    // Reserve enough capacity that rebuildMorseTables() never has to
    // reallocate at audio time.  K = 5 segments + trailing silence;
    // BK = 11 segments + trailing silence.  Round up generously.
    m_morseIntroTable.reserve(16);
    m_morseOutroTable.reserve(32);
}

void ClientQuindarTone::prepare(double sampleRate)
{
    m_sampleRate = sampleRate;
    recacheIfDirty();
    rebuildMorseTables();
    reset();
}

void ClientQuindarTone::setEnabled(bool on) noexcept
{
    m_atomics.enabled.store(on, std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}

bool ClientQuindarTone::isEnabled() const noexcept
{
    return m_atomics.enabled.load(std::memory_order_relaxed);
}

void ClientQuindarTone::setStyle(Style s) noexcept
{
    m_atomics.style.store(static_cast<uint8_t>(s), std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}

ClientQuindarTone::Style ClientQuindarTone::style() const noexcept
{
    return static_cast<Style>(m_atomics.style.load(std::memory_order_relaxed));
}

void ClientQuindarTone::setLevelDb(float db) noexcept
{
    m_atomics.levelDb.store(std::clamp(db, -20.0f, 0.0f),
                            std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}

float ClientQuindarTone::levelDb() const noexcept
{
    return m_atomics.levelDb.load(std::memory_order_relaxed);
}

void ClientQuindarTone::setIntroFreqHz(float hz) noexcept
{
    m_atomics.introFreqHz.store(std::clamp(hz, 400.0f, 3000.0f),
                                std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}

float ClientQuindarTone::introFreqHz() const noexcept
{
    return m_atomics.introFreqHz.load(std::memory_order_relaxed);
}

void ClientQuindarTone::setOutroFreqHz(float hz) noexcept
{
    m_atomics.outroFreqHz.store(std::clamp(hz, 400.0f, 3000.0f),
                                std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}

float ClientQuindarTone::outroFreqHz() const noexcept
{
    return m_atomics.outroFreqHz.load(std::memory_order_relaxed);
}

void ClientQuindarTone::setDurationMs(int ms) noexcept
{
    m_atomics.durationMs.store(std::clamp(ms, 100, 500),
                               std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}

int ClientQuindarTone::durationMs() const noexcept
{
    return m_atomics.durationMs.load(std::memory_order_relaxed);
}

void ClientQuindarTone::setMorseWpm(int wpm) noexcept
{
    m_atomics.morseWpm.store(std::clamp(wpm, 20, 60),
                             std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}

int ClientQuindarTone::morseWpm() const noexcept
{
    return m_atomics.morseWpm.load(std::memory_order_relaxed);
}

void ClientQuindarTone::setMorsePitchHz(float hz) noexcept
{
    m_atomics.morsePitchHz.store(std::clamp(hz, 400.0f, 1200.0f),
                                 std::memory_order_relaxed);
    m_atomics.version.fetch_add(1, std::memory_order_release);
}

float ClientQuindarTone::morsePitchHz() const noexcept
{
    return m_atomics.morsePitchHz.load(std::memory_order_relaxed);
}

int ClientQuindarTone::currentIntroDurationMs() const noexcept
{
    if (style() == Style::Tone) return durationMs();
    // K = 9 dot-units; one dot = 1200 / WPM ms.
    const int wpm = morseWpm();
    return wpm > 0 ? (9 * 1200 / wpm) : 240;
}

int ClientQuindarTone::currentOutroDurationMs() const noexcept
{
    if (style() == Style::Tone) return durationMs();
    // BK = 21 dot-units (B = 9, gap = 3, K = 9).
    const int wpm = morseWpm();
    return wpm > 0 ? (21 * 1200 / wpm) : 560;
}

void ClientQuindarTone::startIntro() noexcept
{
    m_atomics.phase.store(static_cast<uint8_t>(Phase::Engaging),
                          std::memory_order_release);
}

void ClientQuindarTone::startOutro() noexcept
{
    m_atomics.phase.store(static_cast<uint8_t>(Phase::Disengaging),
                          std::memory_order_release);
}

void ClientQuindarTone::forceIdle() noexcept
{
    m_atomics.phase.store(static_cast<uint8_t>(Phase::Idle),
                          std::memory_order_release);
}

bool ClientQuindarTone::coalesceReEngage() noexcept
{
    uint8_t expected = static_cast<uint8_t>(Phase::Disengaging);
    return m_atomics.phase.compare_exchange_strong(
        expected, static_cast<uint8_t>(Phase::Live),
        std::memory_order_acq_rel);
}

ClientQuindarTone::Phase ClientQuindarTone::phase() const noexcept
{
    return static_cast<Phase>(m_atomics.phase.load(std::memory_order_acquire));
}

void ClientQuindarTone::setPhaseCompleteCallback(PhaseCompleteCallback cb)
{
    m_phaseCompleteCb = std::move(cb);
}

void ClientQuindarTone::recacheIfDirty() noexcept
{
    const uint64_t v = m_atomics.version.load(std::memory_order_acquire);
    if (v == m_cachedVersion) return;

    m_cached.enabled = m_atomics.enabled.load(std::memory_order_relaxed);
    m_cached.style = static_cast<Style>(
        m_atomics.style.load(std::memory_order_relaxed));
    m_cached.ampLin = dbToLin(
        m_atomics.levelDb.load(std::memory_order_relaxed));

    const float fs = static_cast<float>(std::max(1.0, m_sampleRate));
    const float introHz = m_atomics.introFreqHz.load(std::memory_order_relaxed);
    const float outroHz = m_atomics.outroFreqHz.load(std::memory_order_relaxed);
    const float morseHz = m_atomics.morsePitchHz.load(std::memory_order_relaxed);
    m_cached.introPhaseInc = kTwoPi * introHz / fs;
    m_cached.outroPhaseInc = kTwoPi * outroHz / fs;
    m_cached.morsePhaseInc = kTwoPi * morseHz / fs;

    const int ms = m_atomics.durationMs.load(std::memory_order_relaxed);
    m_cached.toneFrames = static_cast<int>(
        static_cast<double>(ms) * m_sampleRate / 1000.0);

    const int wpm = m_atomics.morseWpm.load(std::memory_order_relaxed);
    const double dotMs = (wpm > 0) ? (1200.0 / wpm) : 26.667;
    m_cached.morseDotFrames = static_cast<int>(
        dotMs * m_sampleRate / 1000.0);
    // K = 9 dot-units, BK = 21 dot-units.
    m_cached.introMorseFrames = 9  * m_cached.morseDotFrames;
    m_cached.outroMorseFrames = 21 * m_cached.morseDotFrames;

    rebuildMorseTables();
    m_cachedVersion = v;
}

void ClientQuindarTone::rebuildMorseTables() noexcept
{
    // K  = dah dit dah                  ( - · - )
    // BK = dah dit dit dit  +  dah dit dah   ( -··· -·- with inter-letter gap )
    //
    // Element timing (units of 1 dot = morseDotFrames):
    //   dot   = 1u tone
    //   dash  = 3u tone
    //   intra-element gap = 1u silence
    //   inter-letter gap  = 3u silence (only between B and K in BK)
    //
    // K timeline (9 units total):
    //   [3u tone] [1u silence] [1u tone] [1u silence] [3u tone]
    //
    // BK timeline (21 units total):
    //   B = [3u tone] [1u silence] [1u tone] [1u silence] [1u tone]
    //       [1u silence] [1u tone]                              = 9u
    //   inter-letter gap                                         = 3u
    //   K = [3u tone] [1u silence] [1u tone] [1u silence] [3u tone] = 9u

    const int u = m_cached.morseDotFrames;
    if (u <= 0) {
        m_morseIntroTable.clear();
        m_morseOutroTable.clear();
        return;
    }

    auto build = [u](std::vector<MorseSegment>& out,
                     std::initializer_list<std::pair<int, bool>> segs) {
        out.clear();
        int frame = 0;
        for (const auto& [units, toneOn] : segs) {
            MorseSegment seg;
            seg.startFrame   = frame;
            seg.lengthFrames = units * u;
            seg.toneOn       = toneOn;
            out.push_back(seg);
            frame += seg.lengthFrames;
        }
    };

    // K = dah  gap dit  gap dah
    build(m_morseIntroTable, {
        {3, true}, {1, false}, {1, true}, {1, false}, {3, true},
    });

    // BK = (dah gap dit gap dit gap dit) gap-letter (dah gap dit gap dah)
    build(m_morseOutroTable, {
        {3, true}, {1, false}, {1, true}, {1, false}, {1, true},
        {1, false}, {1, true},
        {3, false},                      // inter-letter gap (3u)
        {3, true}, {1, false}, {1, true}, {1, false}, {3, true},
    });
}

void ClientQuindarTone::reset() noexcept
{
    m_phaseFrame   = 0;
    m_sinePhase    = 0.0f;
    m_currentPhase = Phase::Idle;
    m_localPhaseFrame   = 0;
    m_localSinePhase    = 0.0f;
    m_localCurrentPhase = Phase::Idle;
}

void ClientQuindarTone::processSidetone(float* stereoOut, int frames,
                                         double sidetoneSampleRate) noexcept
{
    if (!stereoOut || frames <= 0 || sidetoneSampleRate <= 0.0) return;
    recacheIfDirty();
    if (!m_cached.enabled) {
        m_localCurrentPhase = Phase::Idle;
        m_localPhaseFrame   = 0;
        m_localSinePhase    = 0.0f;
        return;
    }

    const Phase requestedPhase = static_cast<Phase>(
        m_atomics.phase.load(std::memory_order_acquire));

    // Detect phase transitions on the local side independently of the
    // TX-path process().  When the TX path advances the atomic phase
    // (e.g. Engaging → Live), this side picks up the new state on the
    // next call and resets its local counter so a click-free tone
    // segment starts at frame 0.
    if (requestedPhase != m_localCurrentPhase) {
        m_localCurrentPhase = requestedPhase;
        m_localPhaseFrame   = 0;
        m_localSinePhase    = 0.0f;
    }

    if (m_localCurrentPhase == Phase::Idle
        || m_localCurrentPhase == Phase::Live) {
        return;
    }

    // Recompute per-rate frame counts and phase increments — different
    // sample rate from the TX path, so we can't reuse m_cached's frame
    // sizes.  Cheap to do here per block.
    if (sidetoneSampleRate != m_localSampleRate) {
        m_localSampleRate = sidetoneSampleRate;
    }

    const bool isIntro = (m_localCurrentPhase == Phase::Engaging);
    const Style st     = m_cached.style;

    const int totalFrames = (st == Style::Tone)
        ? static_cast<int>(static_cast<double>(
              m_atomics.durationMs.load(std::memory_order_relaxed))
              * m_localSampleRate / 1000.0)
        : (isIntro
            ? static_cast<int>(9  * (1200.0 / m_atomics.morseWpm
                  .load(std::memory_order_relaxed)) * m_localSampleRate / 1000.0)
            : static_cast<int>(21 * (1200.0 / m_atomics.morseWpm
                  .load(std::memory_order_relaxed)) * m_localSampleRate / 1000.0));

    const float toneFreq = isIntro
        ? m_atomics.introFreqHz.load(std::memory_order_relaxed)
        : m_atomics.outroFreqHz.load(std::memory_order_relaxed);
    const float morseFreq = m_atomics.morsePitchHz.load(std::memory_order_relaxed);
    const float fs        = static_cast<float>(m_localSampleRate);
    const float phaseInc  = (st == Style::Tone)
        ? (kTwoPi * toneFreq  / fs)
        : (kTwoPi * morseFreq / fs);

    const int rampFrames = std::max(1,
        static_cast<int>(0.005 * m_localSampleRate));
    const int dotFrames = static_cast<int>(
        (1200.0 / std::max(1, m_atomics.morseWpm
            .load(std::memory_order_relaxed))) * m_localSampleRate / 1000.0);

    // Morse element timeline at the local sample rate.  Reuse the
    // K / BK structure from the TX-path table: same units (1/3/etc)
    // just scaled to local-rate frames.
    auto morseEnvelope = [&](int frame) -> float {
        // K = dah(3)·gap(1)·dit(1)·gap(1)·dah(3)         = 9 units
        // BK = dah(3)·gap(1)·dit(1)·gap(1)·dit(1)·gap(1)·dit(1)
        //      ·gap-letter(3)·dah(3)·gap(1)·dit(1)·gap(1)·dah(3)  = 21 units
        struct Seg { int unitStart; int unitLen; bool toneOn; };
        static constexpr Seg kK[]  = {
            {0, 3, true}, {3, 1, false}, {4, 1, true},
            {5, 1, false}, {6, 3, true}
        };
        static constexpr Seg kBK[] = {
            {0,  3, true},  {3,  1, false}, {4,  1, true},
            {5,  1, false}, {6,  1, true},  {7,  1, false},
            {8,  1, true},  {9,  3, false}, {12, 3, true},
            {15, 1, false}, {16, 1, true},  {17, 1, false},
            {18, 3, true}
        };
        const Seg* table = isIntro ? kK : kBK;
        const int  count = isIntro
            ? static_cast<int>(sizeof(kK)  / sizeof(Seg))
            : static_cast<int>(sizeof(kBK) / sizeof(Seg));

        for (int i = 0; i < count; ++i) {
            const int segStart = table[i].unitStart * dotFrames;
            const int segLen   = table[i].unitLen   * dotFrames;
            if (frame >= segStart && frame < segStart + segLen) {
                if (!table[i].toneOn) return 0.0f;
                const int frameInSeg = frame - segStart;
                const float rIn  = cosSquaredRamp(frameInSeg, rampFrames);
                const float rOut = cosSquaredRamp(segLen - 1 - frameInSeg,
                                                   rampFrames);
                return std::min(rIn, rOut);
            }
        }
        return 0.0f;
    };

    for (int f = 0; f < frames; ++f) {
        if (m_localPhaseFrame >= totalFrames) {
            // Local path mirrors completion but doesn't transition the
            // atomic — the TX path will do that itself when it consumes
            // its own frame counter.  Just stop generating samples.
            m_localCurrentPhase = (m_localCurrentPhase == Phase::Engaging)
                ? Phase::Live : Phase::Idle;
            return;
        }

        float sample = 0.0f;
        if (st == Style::Tone) {
            const float env = envelopeAt(m_localPhaseFrame, totalFrames,
                                          rampFrames);
            sample = std::sin(m_localSinePhase) * env * m_cached.ampLin;
        } else {
            const float env = morseEnvelope(m_localPhaseFrame);
            if (env > 0.0f) {
                sample = std::sin(m_localSinePhase) * env * m_cached.ampLin;
            }
        }
        m_localSinePhase += phaseInc;
        if (m_localSinePhase > kTwoPi) m_localSinePhase -= kTwoPi;

        // Mix additively into stereo output (same sample on both channels).
        stereoOut[f * 2]     += sample;
        stereoOut[f * 2 + 1] += sample;
        ++m_localPhaseFrame;
    }
}

float ClientQuindarTone::generateToneSample(int phaseFrame, int totalFrames,
                                            float phaseInc) noexcept
{
    const int rampFrames = rampFramesFor(m_sampleRate);
    const float env = envelopeAt(phaseFrame, totalFrames, rampFrames);
    const float s = std::sin(m_sinePhase) * env * m_cached.ampLin;
    m_sinePhase += phaseInc;
    if (m_sinePhase > kTwoPi) m_sinePhase -= kTwoPi;
    return s;
}

float ClientQuindarTone::generateMorseSample(
    int phaseFrame,
    const std::vector<MorseSegment>& table,
    int /*totalFrames*/) noexcept
{
    if (table.empty()) {
        m_sinePhase += m_cached.morsePhaseInc;
        if (m_sinePhase > kTwoPi) m_sinePhase -= kTwoPi;
        return 0.0f;
    }

    // Find the segment containing phaseFrame.  Linear scan is fine —
    // tables hold ≤ 13 segments and we hit them in order.
    const MorseSegment* active = nullptr;
    for (const auto& seg : table) {
        if (phaseFrame >= seg.startFrame
            && phaseFrame < seg.startFrame + seg.lengthFrames) {
            active = &seg;
            break;
        }
    }

    float sample = 0.0f;
    if (active && active->toneOn) {
        const int rampFrames = rampFramesFor(m_sampleRate);
        const int frameInSeg = phaseFrame - active->startFrame;
        const float env = envelopeAt(frameInSeg, active->lengthFrames,
                                     rampFrames);
        sample = std::sin(m_sinePhase) * env * m_cached.ampLin;
    }
    m_sinePhase += m_cached.morsePhaseInc;
    if (m_sinePhase > kTwoPi) m_sinePhase -= kTwoPi;
    return sample;
}

void ClientQuindarTone::process(float* interleaved, int frames, int channels) noexcept
{
    if (!interleaved || frames <= 0 || channels < 1 || channels > 2) return;
    recacheIfDirty();

    if (!m_cached.enabled) {
        // Disabled — keep phase Idle so coordinator transitions are
        // benign, but never modify samples.  Force the atomic back to
        // Idle even if our local cached phase is already Idle, since
        // the coordinator may have published a non-Idle phase that we
        // should reflect-and-clear before the next enable.
        m_currentPhase = Phase::Idle;
        m_phaseFrame   = 0;
        m_sinePhase    = 0.0f;
        m_atomics.phase.store(static_cast<uint8_t>(Phase::Idle),
                              std::memory_order_release);
        return;
    }

    const Phase requestedPhase = static_cast<Phase>(
        m_atomics.phase.load(std::memory_order_acquire));

    // Phase transition detection — reset frame counter + sine phase
    // so each phase starts cleanly from t=0 (avoids carry-over click).
    if (requestedPhase != m_currentPhase) {
        m_currentPhase = requestedPhase;
        m_phaseFrame   = 0;
        m_sinePhase    = 0.0f;
    }

    if (m_currentPhase == Phase::Idle || m_currentPhase == Phase::Live) {
        return;
    }

    const bool isIntro = (m_currentPhase == Phase::Engaging);
    const Style st     = m_cached.style;
    const int totalFrames = (st == Style::Tone)
        ? m_cached.toneFrames
        : (isIntro ? m_cached.introMorseFrames : m_cached.outroMorseFrames);
    const float phaseInc = isIntro
        ? m_cached.introPhaseInc
        : m_cached.outroPhaseInc;
    const auto& morseTable = isIntro ? m_morseIntroTable : m_morseOutroTable;

    for (int f = 0; f < frames; ++f) {
        if (m_phaseFrame >= totalFrames) {
            // Phase finished — atomic transition to next phase.  Audio
            // thread is the source of truth for the transition.
            const Phase next = isIntro ? Phase::Live : Phase::Idle;
            m_currentPhase = next;
            m_atomics.phase.store(static_cast<uint8_t>(next),
                                  std::memory_order_release);
            if (m_phaseCompleteCb) {
                // Invoke synchronously — for tests + the
                // QueuedConnection-based dispatcher in TransmitModel.
                const Phase finished = isIntro
                    ? Phase::Engaging : Phase::Disengaging;
                m_phaseCompleteCb(finished);
            }
            // Don't generate further samples this block — let the new
            // phase (Live or Idle) pass through unchanged.
            return;
        }

        const float sample = (st == Style::Tone)
            ? generateToneSample(m_phaseFrame, totalFrames, phaseInc)
            : generateMorseSample(m_phaseFrame, morseTable, totalFrames);

        for (int c = 0; c < channels; ++c) {
            interleaved[f * channels + c] = sample;
        }
        ++m_phaseFrame;
    }
}

void ClientQuindarTone::process(int16_t* interleaved, int frames, int channels) noexcept
{
    if (!interleaved || frames <= 0 || channels < 1 || channels > 2) return;
    recacheIfDirty();

    if (!m_cached.enabled) {
        if (m_currentPhase != Phase::Idle) {
            m_currentPhase = Phase::Idle;
            m_phaseFrame   = 0;
            m_sinePhase    = 0.0f;
            m_atomics.phase.store(static_cast<uint8_t>(Phase::Idle),
                                  std::memory_order_release);
        }
        return;
    }

    const Phase requestedPhase = static_cast<Phase>(
        m_atomics.phase.load(std::memory_order_acquire));

    if (requestedPhase != m_currentPhase) {
        m_currentPhase = requestedPhase;
        m_phaseFrame   = 0;
        m_sinePhase    = 0.0f;
    }

    if (m_currentPhase == Phase::Idle || m_currentPhase == Phase::Live) {
        return;
    }

    const bool isIntro = (m_currentPhase == Phase::Engaging);
    const Style st     = m_cached.style;
    const int totalFrames = (st == Style::Tone)
        ? m_cached.toneFrames
        : (isIntro ? m_cached.introMorseFrames : m_cached.outroMorseFrames);
    const float phaseInc = isIntro
        ? m_cached.introPhaseInc
        : m_cached.outroPhaseInc;
    const auto& morseTable = isIntro ? m_morseIntroTable : m_morseOutroTable;

    for (int f = 0; f < frames; ++f) {
        if (m_phaseFrame >= totalFrames) {
            const Phase next = isIntro ? Phase::Live : Phase::Idle;
            m_currentPhase = next;
            m_atomics.phase.store(static_cast<uint8_t>(next),
                                  std::memory_order_release);
            if (m_phaseCompleteCb) {
                const Phase finished = isIntro
                    ? Phase::Engaging : Phase::Disengaging;
                m_phaseCompleteCb(finished);
            }
            return;
        }

        const float sample = (st == Style::Tone)
            ? generateToneSample(m_phaseFrame, totalFrames, phaseInc)
            : generateMorseSample(m_phaseFrame, morseTable, totalFrames);

        const int16_t s16 = static_cast<int16_t>(std::clamp(
            sample * 32767.0f, -32768.0f, 32767.0f));
        for (int c = 0; c < channels; ++c) {
            interleaved[f * channels + c] = s16;
        }
        ++m_phaseFrame;
    }
}

} // namespace MasterSDR
