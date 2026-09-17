#pragma once
#include <QString>
#include "DeepFistCommitter.h"
#include <vector>
namespace lyra::dsp { class DeepFistModel; }
namespace AetherSDR {
// One worker owns this fixed-memory 3.2 kHz streaming context.
class DeepFistStream {
public:
    // Explicit developer replay configuration; the application uses these defaults.
    struct Parameters {
        int tickSamples = 1280;
        double guardSeconds = 1.3;
        double slowGuardSeconds = 2.2;
        int slowBelowWpm = 13;
        float activityThreshold = 12.f;
        bool requireCompletedMark = false;
        bool carryPending = false;
        bool normalizeActivity = false; // Developer candidate; does not alter inference audio.
        int recentActivitySamples = 0; // Diagnostic candidate; zero keeps the six-second gate.
        bool valid() const;
    };
    DeepFistStream() = default;
    explicit DeepFistStream(const Parameters& parameters);
    struct Observation {
        double seconds;
        int realSamples;
        float paddedRatio;
        float realRatio;
        int wpm;
        double settled;
        bool completedMark;
        double committedBefore = 0;
        double committedAfter = 0;
        bool gated = false;
        QString publication;
        float recentRatio = 0.f;
        struct Token {
            int id;
            int frame;
            double seconds;
            QString text;
            QString decision;
        };
        std::vector<Token> tokens;
    };
    QString process(const float* samples, int count, lyra::dsp::DeepFistModel& model,
                    std::vector<Observation>* observations = nullptr);
    static bool hasCompletedMark(const float* samples, int count);
    bool failed() const { return m_failed; }
private:
    static constexpr int kWindow = 19200;
    Parameters m_parameters;
    std::vector<float> m_ring = std::vector<float>(kWindow, 0.f);
    std::vector<float> m_window = std::vector<float>(kWindow, 0.f);
    std::vector<float> m_logits;
    quint64 m_samples = 0;
    DeepFistCommitter m_committer;
    bool m_failed = false;
};
}
