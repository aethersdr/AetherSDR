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
    if (m_vad != nullptr) {
        m_vad->reset();
    }
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

void AsrSegmenter::closeSegment(std::vector<std::vector<float>>& out)
{
    // Gate on speech content, not total length: the segment also carries the
    // hangover silence, which must not count toward the minimum-speech floor.
    if (m_speechSamples >= m_minSpeechSamples) {
        out.push_back(std::move(m_segment));
    }
    m_segment.clear();
    m_inSpeech = false;
    m_trailingSilence = 0;
    m_speechSamples = 0;
}

std::vector<std::vector<float>> AsrSegmenter::feed(const float* samples, int count)
{
    std::vector<std::vector<float>> out;

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
            m_segment.insert(m_segment.end(), m_frame.begin(), m_frame.end());
            m_speechSamples += static_cast<int>(m_frame.size());
        } else if (m_inSpeech) {
            // Keep the silence inside the segment until hangover closes it, so
            // brief inter-word gaps don't split an utterance.
            m_segment.insert(m_segment.end(), m_frame.begin(), m_frame.end());
            m_trailingSilence += static_cast<int>(m_frame.size());
            if (m_trailingSilence >= m_hangoverSamples) {
                closeSegment(out);
            }
        }
        // else: silence outside speech — ignored.

        // Hard cap on a single utterance.
        if (m_inSpeech && static_cast<int>(m_segment.size()) >= m_maxSegmentSamples) {
            closeSegment(out);
        }

        m_frame.clear();
    }

    return out;
}

std::vector<std::vector<float>> AsrSegmenter::flush()
{
    std::vector<std::vector<float>> out;
    // Fold any partial frame into the segment before closing.
    if (m_inSpeech && !m_frame.empty()) {
        m_segment.insert(m_segment.end(), m_frame.begin(), m_frame.end());
    }
    m_frame.clear();
    if (m_inSpeech) {
        closeSegment(out);
    }
    return out;
}

} // namespace AetherSDR
