#include "asr/AsrSegmenter.h"

#include "asr/IVad.h"

#include <cmath>

namespace AetherSDR {

AsrSegmenter::AsrSegmenter(const Config& config)
    : m_config(config)
{
    m_frameSamples = framesToSamples(m_config.frameMs);
    if (m_frameSamples < 1) {
        m_frameSamples = 1;
    }
    m_minSpeechSamples = framesToSamples(m_config.minSpeechMs);
    m_hangoverSamples = framesToSamples(m_config.hangoverMs);
    m_maxSegmentSamples = framesToSamples(m_config.maxSegmentMs);
    m_longGapSamples = framesToSamples(m_config.longGapMs);
    m_frame.reserve(m_frameSamples);
}

int AsrSegmenter::framesToSamples(int ms) const
{
    return static_cast<int>(static_cast<long long>(ms) * m_config.sampleRate / 1000);
}

void AsrSegmenter::reset()
{
    m_frame.clear();
    m_segment.clear();
    m_inSpeech = false;
    m_trailingSilence = 0;
    m_speechSamples = 0;
    m_pendingContinues = false;
    m_pendingOverlapMs = 0;
    m_idleSilence = 0;
    m_longGapFired = false;
    m_longGapPending = false;
    if (m_vad != nullptr) {
        m_vad->reset();
    }
}

bool AsrSegmenter::consumeLongGap()
{
    const bool r = m_longGapPending;
    m_longGapPending = false;
    return r;
}

void AsrSegmenter::setMaxSegmentMs(int ms)
{
    m_config.maxSegmentMs = ms;
    m_maxSegmentSamples = framesToSamples(ms);
    if (m_maxSegmentSamples < m_frameSamples) {
        m_maxSegmentSamples = m_frameSamples;
    }
}

void AsrSegmenter::setSpeechRms(float rms)
{
    // Read live in feed(); takes effect on the next frame.
    m_config.speechRms = rms;
}

void AsrSegmenter::setHangoverMs(int ms)
{
    m_config.hangoverMs = ms;
    m_hangoverSamples = framesToSamples(ms);
}

void AsrSegmenter::closeSegment(std::vector<ClosedSegment>& out, bool forceCap)
{
    // Gate on speech content, not total length: the segment also carries the
    // hangover silence, which must not count toward the minimum-speech floor.
    const bool qualifies = m_speechSamples >= m_minSpeechSamples;

    // Continue the same utterance across this close only when it's the
    // maxSegmentMs cap firing (speech still ongoing, not real silence), overlap
    // is configured, the segment closing now actually qualifies (an unqualified
    // segment's audio is discarded, not emitted — nothing to carry), AND the cut
    // lands mid-speech (m_trailingSilence == 0). If the cap fires during the
    // hangover, the trailing window is silence: there is no split word to
    // recover, and carrying it would wrongly count that silence as speech.
    const bool continueUtterance =
        forceCap && m_config.overlapMs > 0 && qualifies && m_trailingSilence == 0;

    // Snapshot the trailing window BEFORE m_segment is moved out below.
    std::vector<float> carry;
    int carryMs = 0;
    if (continueUtterance) {
        int overlapSamples =
            std::min(framesToSamples(m_config.overlapMs), static_cast<int>(m_segment.size()));
        // Bound the carry to HALF the segment budget so the reseeded segment is
        // always at least half fresh audio before the next cap. Without this a
        // large overlap (≥ maxSegmentMs — both are user sliders) would carry
        // nearly the whole segment forward every cap, force-decoding the same
        // audio over and over (a runaway backlog through the backpressure-free
        // pushAudio path). Half-budget caps the intrinsic overlap cost at ~2×
        // realtime for every slider combination.
        overlapSamples = std::min(overlapSamples, m_maxSegmentSamples / 2);
        if (overlapSamples > 0) {
            carry.assign(m_segment.end() - overlapSamples, m_segment.end());
            carryMs = static_cast<int>(static_cast<long long>(overlapSamples) * 1000
                                       / m_config.sampleRate);
        }
    }

    if (qualifies) {
        ClosedSegment cs;
        cs.samples = std::move(m_segment);
        cs.continuesPrevious = m_pendingContinues; // this segment itself began with carried audio
        cs.overlapMs = m_pendingContinues ? m_pendingOverlapMs : 0; // window carried into THIS segment
        out.push_back(std::move(cs));
    }

    m_segment.clear();
    m_trailingSilence = 0;
    m_speechSamples = 0;
    m_pendingContinues = false;
    m_pendingOverlapMs = 0;

    if (!carry.empty()) {
        // Still mid-utterance: seed the next segment with the carried audio
        // instead of starting from silence, and mark it (with the window size in
        // force now) so the worker de-dups the repeated leading words against the
        // right budget. m_inSpeech stays true.
        //
        // The carry counts toward m_speechSamples deliberately, reversing the
        // hazard note in #4821's triage (maintainer-ruled on the RFC before this
        // landed). Not counting it would mean a continuation carrying less than
        // minSpeechMs of NEW speech never qualifies — and the second half of the
        // word split at the cap is exactly that case, so the floor would discard
        // the fragment this whole feature exists to recover. The price is the
        // documented Approach-A margin: if the speaker stops immediately after
        // the cap, the continuation clears the floor on carried audio alone and
        // emits one decode that is entirely the previous tail, recovered only
        // when the worker's de-dup strips it back to empty.
        m_speechSamples = static_cast<int>(carry.size());
        m_pendingContinues = true;
        m_pendingOverlapMs = carryMs;
        m_segment = std::move(carry);
    } else {
        m_inSpeech = false;
    }
}

std::vector<AsrSegmenter::ClosedSegment> AsrSegmenter::feed(const float* samples, int count)
{
    std::vector<ClosedSegment> out;

    for (int i = 0; i < count; ++i) {
        m_frame.push_back(samples[i]);
        if (static_cast<int>(m_frame.size()) < m_frameSamples) {
            continue;
        }

        // Decide speech/silence for this frame: a plugged-in VAD (e.g. Silero)
        // if present, else the built-in RMS energy threshold.
        bool speechFrame;
        if (m_vad != nullptr) {
            speechFrame = m_vad->isSpeech(m_frame.data(), static_cast<int>(m_frame.size()));
        } else {
            double sumSq = 0.0;
            for (const float s : m_frame) {
                sumSq += static_cast<double>(s) * s;
            }
            const float rms = static_cast<float>(std::sqrt(sumSq / m_frame.size()));
            speechFrame = rms >= m_config.speechRms;
        }

        if (speechFrame) {
            if (!m_inSpeech) {
                m_inSpeech = true;
                m_segment.clear();
            }
            m_trailingSilence = 0;
            // Speech resumed: the idle gap (if any) is over; re-arm for the next.
            m_idleSilence = 0;
            m_longGapFired = false;
            m_segment.insert(m_segment.end(), m_frame.begin(), m_frame.end());
            m_speechSamples += static_cast<int>(m_frame.size());
        } else if (m_inSpeech) {
            // Keep the silence inside the segment until hangover closes it, so
            // brief inter-word gaps don't split an utterance.
            m_segment.insert(m_segment.end(), m_frame.begin(), m_frame.end());
            m_trailingSilence += static_cast<int>(m_frame.size());
            if (m_trailingSilence >= m_hangoverSamples) {
                closeSegment(out, /*forceCap=*/false);
            }
        } else {
            // Silence outside speech: accumulate the idle gap. When it first
            // reaches longGapMs, latch a one-shot the worker consumes to flush
            // carried context (fires once per gap; re-armed when speech resumes).
            if (m_longGapSamples > 0 && !m_longGapFired) {
                m_idleSilence += static_cast<int>(m_frame.size());
                if (m_idleSilence >= m_longGapSamples) {
                    m_longGapFired = true;
                    m_longGapPending = true;
                }
            }
        }

        // Hard cap on a single utterance.
        if (m_inSpeech && static_cast<int>(m_segment.size()) >= m_maxSegmentSamples) {
            closeSegment(out, /*forceCap=*/true);
        }

        m_frame.clear();
    }

    return out;
}

std::vector<AsrSegmenter::ClosedSegment> AsrSegmenter::flush()
{
    std::vector<ClosedSegment> out;
    // Fold any partial frame into the segment before closing.
    if (m_inSpeech && !m_frame.empty()) {
        m_segment.insert(m_segment.end(), m_frame.begin(), m_frame.end());
    }
    m_frame.clear();
    if (m_inSpeech) {
        // End of stream: never a candidate to continue via overlap (there's no
        // next segment to carry into).
        closeSegment(out, /*forceCap=*/false);
    }
    return out;
}

} // namespace AetherSDR
