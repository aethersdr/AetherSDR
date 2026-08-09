#include "CwSidetoneGenerator.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include <QVarLengthArray>

namespace AetherSDR {

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
    // Several threads legitimately produce edges — the iambic and CWX
    // workers call in directly, and the GUI thread echoes every
    // radio-bound edge via RadioModel::cwKeyDownChanged — so slot write
    // and head publish must be exclusive among producers.  A short spin
    // is cheaper than any blocking primitive at tens of edges per
    // second; process() only consumes the tail and never takes this
    // lock, so the audio thread stays wait-free.  The timestamp is taken
    // inside the lock so queue order equals timestamp order — process()
    // relies on that for its edges-are-time-ordered early-out.
    // Worst case among producers: the GUI thread descheduled inside the
    // section leaves a keying worker spinning until the holder resumes.
    // The section is ~20 instructions, so the window is vanishingly
    // small, and the cost is one edge timestamped late by the wait —
    // the same per-edge epsilon class as wake latency, and never a
    // correctness concern.
    while (m_edgeLock.test_and_set(std::memory_order_acquire)) { /* spin */ }
    const auto now = std::chrono::steady_clock::now();
    const uint32_t head = m_edgeHead.load(std::memory_order_relaxed);
    const uint32_t tail = m_edgeTail.load(std::memory_order_acquire);
    // Mirror the latest state BEFORE publishing the head.  The lock excludes
    // other producers but not the audio thread, so a producer descheduled
    // mid-section is observable either way — this order makes it harmless.
    // Mirror ahead of the queue: process() applies it at the next block start
    // (the pre-#4809 behavior, and what rescues the final key-up on queue
    // overflow) and the queued edge is then a no-op through applyKeyEdge().
    // The reverse order lets the consumer advance m_gateDown past a stale
    // mirror, and the same fallback then injects an inverted edge — a dropout
    // mid-element, plus a second phantom edge undoing it.
    m_keyDown.store(down, std::memory_order_relaxed);
    if (head - tail < kEdgeQueueSize) {
        m_edgeQueue[head % kEdgeQueueSize] = {now, down};
        m_edgeHead.store(head + 1, std::memory_order_release);
    }
    m_edgeLock.clear(std::memory_order_release);
}

void CwSidetoneGenerator::reset() noexcept
{
    m_state = State::Idle;
    m_rampSample = 0;
    m_phase = 0.0;
    m_gateDown = false;
    m_haveAnchor = false;
    m_streamPos = 0;
    m_idleSamples = 0;
    // Drop queued edges (consumer-side drain: only the tail moves).
    m_edgeTail.store(m_edgeHead.load(std::memory_order_acquire),
                     std::memory_order_release);
}

void CwSidetoneGenerator::setSampleRateHz(int hz) noexcept
{
    m_sampleRateHz = (hz > 0) ? hz : 48000;
    m_rampLength = std::max(1, static_cast<int>(m_shapingMs.load() *
                                                m_sampleRateHz / 1000.0f));
    reset();
}

// The state-machine edge transitions, factored out of process() so they
// can fire at an exact sample offset mid-block rather than only at the
// block start (#4809).  The re-entrant ramp mirroring is unchanged.
void CwSidetoneGenerator::applyKeyEdge(bool down) noexcept
{
    m_gateDown = down;
    switch (m_state) {
    case State::Idle:
        if (down) {
            m_state = State::RampUp;
            m_rampSample = 0;
        }
        break;
    case State::RampUp:
        if (!down) {
            m_state = State::RampDown;
            // Continue from the current envelope position so a quick
            // dot doesn't audibly click — start ramp-down from where
            // ramp-up left off, scaled to the same envelope value.
            m_rampSample = m_rampLength - m_rampSample;
        }
        break;
    case State::Sustain:
        if (!down) {
            m_state = State::RampDown;
            m_rampSample = 0;
        }
        break;
    case State::RampDown:
        if (down) {
            // Mirror image of RampUp→RampDown: re-enter from current
            // envelope position.
            m_state = State::RampUp;
            m_rampSample = m_rampLength - m_rampSample;
        }
        break;
    }
}

bool CwSidetoneGenerator::process(float* out, int frames) noexcept
{
    const bool tapSet = static_cast<bool>(m_sampleTap);

    if (!m_enabled.load(std::memory_order_relaxed)) {
        // Disabled — bring state back to idle on next block so a flip-on
        // mid-keying starts cleanly from silence.  reset() also drops
        // queued edges, so stale timestamps can't fire on re-enable.
        if (m_state != State::Idle || m_haveAnchor)
            reset();
        else
            m_edgeTail.store(m_edgeHead.load(std::memory_order_acquire),
                             std::memory_order_release);
        if (tapSet) {
            // Mirror silence to the TX-decode tap so the downstream
            // decoder's timeline doesn't jump forward across the gap.
            QVarLengthArray<float, 1024> silence(frames);
            std::memset(silence.data(), 0, frames * sizeof(float));
            m_sampleTap(silence.data(), frames, m_sampleRateHz);
        }
        return false;
    }

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

    // ── Collect this block's key edges at exact sample offsets ────────
    // Map each queued edge's timestamp into the sample stream via the
    // burst anchor.  Edges that fall beyond this block stay queued.
    const int64_t blockStart = m_streamPos;
    const int64_t blockEnd   = blockStart + frames;
    // Prealloc covers a full ring drain after a scheduler stall, so the
    // audio callback never heap-allocates here.
    QVarLengthArray<std::pair<int, bool>, kEdgeQueueSize> blockEdges;

    // ── Keep the mapping honest about real time ───────────────────────
    // Everything below assumes process() is pumped in real time, so that
    // m_streamPos and the anchor's wall clock advance together.  Not every
    // consumer is: the recorder's sidetone generator renders only while the
    // radio is transmitting a CW over (AudioEngine::onCwRecordPump), while
    // setKeyDown() keeps queueing from every paddle edge regardless.  Left
    // alone, that generator anchors on keying from seconds or minutes ago
    // and replays it into the next over, and — when the pump stops with the
    // gate still down — never reaches the Idle branch below that would have
    // released the anchor, so it renders one unbroken tone instead.
    //
    // Two guards, both bounded by the same idle threshold that governs a
    // normal re-anchor.  One clock read per block, and only while there is
    // an anchor or something queued.
    const bool haveQueued = m_edgeTail.load(std::memory_order_relaxed)
                            != m_edgeHead.load(std::memory_order_acquire);
    if (m_haveAnchor || haveQueued) {
        const auto nowTp = std::chrono::steady_clock::now();
        const int64_t idleLimit =
            static_cast<int64_t>(m_sampleRateHz) * kReanchorIdleMs / 1000;
        auto toSamples = [this](std::chrono::steady_clock::duration d) {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count()
                   * m_sampleRateHz / 1'000'000'000LL;
        };
        // (1) The mapping itself has gone stale — wall clock has run ahead of
        // the samples we have rendered by more than a re-anchor's worth,
        // which only happens when process() stopped being called.  Tested
        // one-sided on purpose: a stream position ahead of wall clock is
        // ordinary prefill (the QAudioSink path fills a 50 ms buffer up
        // front) and must not re-anchor.
        if (m_haveAnchor) {
            const int64_t expected = m_anchorPos + toSamples(nowTp - m_anchorTime);
            if (expected - blockStart > idleLimit)
                m_haveAnchor = false;
        }
        // (2) About to anchor afresh: drop edges old enough to belong to a
        // previous keying sequence rather than replaying them here.  The
        // retained m_keyDown mirror still delivers the true current state
        // through the fallback below, so nothing that matters is lost.
        if (!m_haveAnchor) {
            for (;;) {
                const uint32_t tail = m_edgeTail.load(std::memory_order_relaxed);
                if (tail == m_edgeHead.load(std::memory_order_acquire))
                    break;
                if (toSamples(nowTp - m_edgeQueue[tail % kEdgeQueueSize].t)
                        <= idleLimit)
                    break;
                m_edgeTail.store(tail + 1, std::memory_order_release);
            }
        }
    }

    for (;;) {
        const uint32_t tail = m_edgeTail.load(std::memory_order_relaxed);
        if (tail == m_edgeHead.load(std::memory_order_acquire))
            break;
        const KeyEdge e = m_edgeQueue[tail % kEdgeQueueSize];
        if (!m_haveAnchor) {
            // First edge of a burst plays at this block's start — the
            // same onset latency as the old block-polling gate; every
            // later edge lands relative to it, sample-exact.
            m_anchorTime = e.t;
            m_anchorPos  = blockStart;
            m_haveAnchor = true;
        }
        const int64_t target = m_anchorPos +
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                e.t - m_anchorTime).count() * m_sampleRateHz / 1'000'000'000LL;
        if (target >= blockEnd)
            break;  // future block — edges are time-ordered, stop here
        const int off = static_cast<int>(
            std::max<int64_t>(target, blockStart) - blockStart);
        blockEdges.append({off, e.down});
        m_edgeTail.store(tail + 1, std::memory_order_release);
    }
    // Overflow / missed-edge fallback: nothing queued but the last-known
    // state disagrees with the gate — apply it at the block start (the
    // pre-#4809 behavior), so the final key-up can never be lost.
    if (blockEdges.isEmpty()
        && m_edgeTail.load(std::memory_order_relaxed)
               == m_edgeHead.load(std::memory_order_acquire)
        && m_keyDown.load(std::memory_order_relaxed) != m_gateDown) {
        blockEdges.append({0, m_keyDown.load(std::memory_order_relaxed)});
    }
    if (!blockEdges.isEmpty())
        m_idleSamples = 0;

    if (m_state == State::Idle && blockEdges.isEmpty()) {
        m_streamPos = blockEnd;
        if (m_haveAnchor && !m_gateDown) {
            // Keep the anchor across ordinary inter-element and
            // inter-character gaps — dropping it per-gap would snap the
            // next element's onset back to a block boundary, leaving the
            // rhythm block-quantized while only element lengths were
            // exact.  Only a genuine pause releases the mapping.
            m_idleSamples += frames;
            if (m_idleSamples >=
                static_cast<int64_t>(m_sampleRateHz) * kReanchorIdleMs / 1000)
                m_haveAnchor = false;
        }
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

    int edgeIdx = 0;
    for (int i = 0; i < frames; ++i) {
        // Fire any edges scheduled for this exact sample.
        while (edgeIdx < blockEdges.size() && blockEdges[edgeIdx].first == i) {
            applyKeyEdge(blockEdges[edgeIdx].second);
            ++edgeIdx;
        }
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

    m_streamPos = blockEnd;
    m_lastPitchHz = pitch;
    return wroteAny;
}

} // namespace AetherSDR
