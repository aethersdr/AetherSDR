#pragma once

#include <QString>
#include <QtGlobal>

#include <functional>
#include <memory>

namespace AetherSDR {

// A streaming recogniser that shows provisional text while the selected ASR
// backend is still waiting for its segment to close (docs/asr-copy-assist.md,
// "Live preview"). It never produces Copy Assist's final text.
//
// Positions are 16 kHz sample indices on the caller's timeline (AsrSegmenter's
// samplesFed()). The worker owns the object and calls it from the ASR thread;
// results arrive through LivePreviewCallbacks on any thread.

struct LivePreviewResult {
    qint64 startSample = 0;
    qint64 endSample = 0;
    QString text;
    // Final: the recogniser will not revise this span. Provisional: a later
    // result starting at the same position replaces it.
    bool isFinal = false;
};

enum class LivePreviewState { Preparing, Ready, Failed };

struct LivePreviewCallbacks {
    std::function<void(const LivePreviewResult&)> onResult;
    std::function<void(LivePreviewState, const QString& message)> onState;
};

class ILivePreview {
public:
    virtual ~ILivePreview() = default;
    // Mono 16 kHz samples starting at startSample. Audio fed before the
    // recogniser reports Ready is dropped.
    virtual void feed(const float* samples, int count, qint64 startSample) = 0;
    // Ask for everything up to `sample` to be finalised (asynchronous).
    virtual void finalizeThrough(qint64 sample) = 0;
};

// Builds a live preview wired to the given callbacks. The destructor of the
// returned object guarantees no callback runs after it returns.
using LivePreviewFactory =
    std::function<std::unique_ptr<ILivePreview>(LivePreviewCallbacks callbacks)>;

// Apple SpeechTranscriber (macOS 26+). appleLivePreviewSupported() is false on
// every other platform/OS and when the build has no Swift bridge; the factory
// then returns nullptr.
bool appleLivePreviewSupported();
// Why the preview is unavailable here, for a disabled control's description.
// Empty when it is supported.
QString appleLivePreviewUnavailableReason();
LivePreviewFactory appleLivePreviewFactory(const QString& language);

} // namespace AetherSDR
