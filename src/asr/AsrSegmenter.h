#pragma once

#include <algorithm>
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
        // The speaker-embedding model (.onnx) is deliberately NOT carried here:
        // it is loaded at runtime via AsrEngine::setSpeakerModelPath() so a
        // labeling change never has to rebuild the engine (#4737). Only the
        // clustering threshold, which the worker's clusterer needs at
        // construction, stays in this bundle.
        float speakerThreshold = 0.50f; // cosine threshold for the same speaker
        // Boundary-word recovery / segment-overlap (RFC #4821, opt-in, 0 =
        // disabled). When a segment is force-closed by maxSegmentMs (speech still
        // ongoing, not a real silence gap), carry this many ms of trailing audio
        // forward as the start of the next segment so a word split at the cut
        // isn't lost. Never applied on a hangover-based close, where speech had
        // already stopped (nothing to recover). The worker de-duplicates the
        // repeated leading words at the text level (Approach A) using the
        // continuesPrevious flag below.
        int overlapMs = 0;
        // Idle silence AFTER an utterance's hangover close counts toward this;
        // once it reaches longGapMs the run is a real pause and consumeLongGap()
        // reports it so the worker can flush any carried ASR context and start
        // the next utterance clean. The hangover silence itself is part of the
        // utterance and is NOT counted, so the effective wall-clock pause before
        // a flush is hangoverMs + longGapMs (~2.8 s at the defaults). Fixed, not
        // UI-exposed. 0 disables.
        int longGapMs = 2500;
    };

    // One closed utterance. `continuesPrevious` is true when this segment was
    // seeded with trailing audio carried across the PREVIOUS segment's cap-forced
    // close (segment overlap) — its leading words re-transcribe the tail of that
    // segment, so the consumer must de-duplicate them. false for an ordinary
    // (non-overlapping) segment. `overlapMs` records how much audio was actually
    // carried into this segment (0 when continuesPrevious is false); it is
    // captured at close time so de-dup uses the window in force when the segment
    // closed, not whatever the slider reads later.
    struct ClosedSegment {
        std::vector<float> samples;
        bool continuesPrevious = false;
        int overlapMs = 0;
    };

    AsrSegmenter() : AsrSegmenter(Config{}) {}
    explicit AsrSegmenter(const Config& config);

    // Feed mono float samples in [-1, 1]. Returns any utterances that closed as
    // a result of this input. Usually empty.
    std::vector<ClosedSegment> feed(const float* samples, int count);

    // Close any in-progress utterance (end of stream / mode change). Returns it
    // if it qualifies as speech, otherwise empty. Never carries overlap forward
    // (there's no "next segment" at end of stream).
    std::vector<ClosedSegment> flush();

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
    // Boundary-word recovery window (RFC #4821); see Config::overlapMs. Takes
    // effect on the next cap-forced close. 0 disables.
    void setOverlapMs(int ms) { m_config.overlapMs = std::max(0, ms); }
    int maxSegmentMs() const { return m_config.maxSegmentMs; }
    float speechRms() const { return m_config.speechRms; }
    int hangoverMs() const { return m_config.hangoverMs; }
    int overlapMs() const { return m_config.overlapMs; }

    bool inSpeech() const { return m_inSpeech; }

    // True once per idle-silence gap that reaches Config::longGapMs; clears on
    // read. The worker polls this each feed() and, when set, flushes the
    // backend's carried context so a real pause starts the next utterance clean.
    bool consumeLongGap();

private:
    int framesToSamples(int ms) const;
    // forceCap: true when this close is the maxSegmentMs cap firing mid-speech
    // (a candidate to continue via overlap), false for a real hangover / end-of-
    // stream close (the utterance actually ended; never carries overlap forward).
    void closeSegment(std::vector<ClosedSegment>& out, bool forceCap);

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
    bool m_pendingContinues = false; // next emitted segment was seeded with carried overlap audio
    int m_pendingOverlapMs = 0;      // ms of audio carried into that next segment (for its overlapMs)
    int m_longGapSamples = 0;       // Config::longGapMs in samples (0 = disabled)
    int m_idleSilence = 0;          // contiguous silence samples while NOT in speech
    bool m_longGapFired = false;    // latched when the current idle gap crossed the threshold
    bool m_longGapPending = false;  // consumable one-shot for consumeLongGap()
};

} // namespace AetherSDR
