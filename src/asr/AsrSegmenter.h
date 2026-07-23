#pragma once

#include <string>
#include <vector>

// Energy-based voice-activity segmenter for the ASR pipeline (RFC #4333).
//
// Whisper is a chunk model, not a streaming one, and running it on dead-carrier
// noise wastes cycles. This class turns a continuous 16 kHz mono stream into
// discrete speech utterances ("overs"): it opens a segment when short-term
// energy rises above a threshold and closes it after a hangover of silence (or
// at a hard maximum length), dropping segments too short to be speech. The
// closed segment's samples are then handed to the ASR backend.
//
// Deliberately whisper-free and allocation-simple so it can be unit-tested
// offline with synthetic audio and reused regardless of backend.

namespace AetherSDR {

class IVad;

class AsrSegmenter {
public:
    struct Config {
        int sampleRate = 16000;      // whisper's required rate
        int frameMs = 10;            // energy is evaluated per frame
        float speechRms = 0.010f;    // RMS above this = speech frame
        int minSpeechMs = 200;       // discard utterances shorter than this
        int hangoverMs = 300;        // trailing silence that closes a segment
        int maxSegmentMs = 20000;    // force-close cap (a very long over)
        // Optional Silero VAD model (.onnx). Empty = built-in energy VAD. The
        // worker (not the segmenter) consumes this to build the detector; carried
        // here because Config is the worker's construction bundle.
        std::string vadModelPath;
        // Optional speaker-embedding model (.onnx) for per-utterance speaker
        // labeling (A/B/C…). Empty = no labeling. Also worker-consumed.
        std::string speakerModelPath;
        float speakerThreshold = 0.50f; // cosine threshold for the same speaker
    };

    AsrSegmenter() : AsrSegmenter(Config{}) {}
    explicit AsrSegmenter(const Config& config);

    // Feed mono float samples in [-1, 1]. Returns any utterances that closed as
    // a result of this input (each a contiguous sample buffer). Usually empty.
    std::vector<std::vector<float>> feed(const float* samples, int count);

    // Close any in-progress utterance (end of stream / mode change). Returns it
    // if it qualifies as speech, otherwise empty.
    std::vector<std::vector<float>> flush();

    // Discard all buffered state without emitting.
    void reset();

    // Plug in a voice-activity detector (non-owning). When set, its isSpeech()
    // replaces the built-in energy threshold for the speech/silence decision; the
    // utterance state machine (hangover, min/max) is unchanged. Null = energy VAD.
    void setVad(IVad* vad) { m_vad = vad; }

    // Runtime-adjustable tuning (call on the segmenter's own thread):
    //  - maxSegmentMs: hard force-close cap ("decode buffer" size); a long over
    //    is force-decoded at this length even without a silence gap.
    //  - speechRms: energy threshold above which a frame counts as speech —
    //    lower = more sensitive VAD (picks up fainter/weaker signals).
    //  - hangoverMs: trailing silence that closes an utterance.
    void setMaxSegmentMs(int ms);
    void setSpeechRms(float rms);
    void setHangoverMs(int ms);
    int maxSegmentMs() const { return m_config.maxSegmentMs; }
    float speechRms() const { return m_config.speechRms; }
    int hangoverMs() const { return m_config.hangoverMs; }

    bool inSpeech() const { return m_inSpeech; }

private:
    int framesToSamples(int ms) const;
    void closeSegment(std::vector<std::vector<float>>& out);

    Config m_config;
    int m_frameSamples = 160;
    int m_minSpeechSamples = 0;
    int m_hangoverSamples = 0;
    int m_maxSegmentSamples = 0;

    IVad* m_vad = nullptr;          // optional learned VAD; null = energy threshold
    std::vector<float> m_frame;     // accumulates one frame for RMS
    std::vector<float> m_segment;   // the current utterance
    bool m_inSpeech = false;
    int m_trailingSilence = 0;      // samples of silence since last speech frame
    int m_speechSamples = 0;        // speech-only samples (excludes hangover), gates minSpeech
};

} // namespace AetherSDR
