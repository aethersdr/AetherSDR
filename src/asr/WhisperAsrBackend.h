#pragma once

#include "asr/IAsrBackend.h"

#include <QString>

#include <functional>
#include <memory>
#include <vector>

struct whisper_context;

namespace AetherSDR {

// whisper.cpp implementation of IAsrBackend. CPU inference (the vendored ggml
// is CPU-only for now); language defaults to English but is configurable.
// Lives entirely on AsrEngine's worker thread.
class WhisperAsrBackend : public IAsrBackend {
public:
    WhisperAsrBackend();
    // gpuDevice: which GPU to run on (index among GPU devices; see asrGpuDevices),
    // or -1 to force CPU.
    explicit WhisperAsrBackend(QString language, int gpuDevice = 0);
    ~WhisperAsrBackend() override;

    bool load(const QString& modelPath, QString* error) override;
    bool isLoaded() const override { return m_ctx != nullptr; }
    AsrTranscript transcribe(const std::vector<float>& pcm16k, QString* error) override;
    void unload() override;

private:
    whisper_context* m_ctx = nullptr;
    QString m_language;
    int m_threads = 0;
    int m_gpuDevice = 0;
    // Whether m_ctx was created on the GPU. False after the CPU retry in
    // load(), so a decode-time throw never latches a device that was not
    // involved.
    bool m_ctxOnGpu = false;
};

// A selectable GPU: `index` is the value to pass as gpuDevice (its position among
// GPU/IGPU devices in ggml's enumeration order); `name` is a human description.
// `usable` is the session verdict: false when the device failed the decode
// capability probe, or when a model load on it failed earlier this run.
struct AsrGpuDevice {
    int index = 0;
    QString name;
    bool usable = true;
};

// Factory for wiring AsrEngine to the production whisper backend. Kept here so
// AsrEngine.cpp never references whisper (keeping the engine — and its unit
// test — independent of the vendored library). Matches AsrBackendFactory.
std::function<std::unique_ptr<IAsrBackend>()>
whisperAsrBackendFactory(const QString& language = QStringLiteral("en"), int gpuDevice = 0);

// True when a GPU ggml backend (Vulkan/Metal) is compiled in and a GPU device is
// present. Used to default the model tier and enable GPU inference.
bool asrGpuAvailable();

// All selectable GPU devices (discrete + integrated), in the order whisper's
// gpu_device indexes them. Empty on CPU-only builds / GPU-less hosts.
std::vector<AsrGpuDevice> asrGpuDevices();

// The device index to default to: the first usable device, or -1 (CPU) when none
// is. An unusable device stays selectable — it is simply never chosen for you.
int asrResolveDefaultGpuIndex(const std::vector<AsrGpuDevice>& devices);

// Session failure latch. A GPU whose model load failed once must never be tried
// again in this process: ggml's Vulkan instance state is sticky, so the first
// failure is survivable but a second attempt on the poisoned state can fault
// somewhere no caller can catch. Marking is one-way and lives until restart.
void asrMarkGpuDeviceFailed(int index);
bool asrGpuDeviceFailed(int index);

// After compute-device resolution, which model tier should be running. The
// GPU-default tier is heavy enough that it only makes sense on a usable GPU:
//  - raise to it only while the GPU default is wanted (no explicit operator
//    model choice yet) AND resolution landed on a usable GPU;
//  - walk it back to the base default when it is selected only because an
//    earlier resolution auto-raised it (`gpuDefaultActive`) and resolution has
//    since fallen off the GPU — the heaviest model cannot keep up on CPU,
//    which is the "backlog climbing, no text" shape of #4502;
//  - never touch a tier the operator picked explicitly.
// Returns the tier to run plus the updated auto-raise state. Header-inline and
// whisper-free for the same reason as asrLanguageOrDefault below: unit
// testable without linking the vendored library.
struct AsrTierResolution {
    QString tierId;
    bool gpuDefaultActive = false;
};

inline AsrTierResolution asrReconcileDefaultTier(const QString& currentTier,
                                                 bool wantGpuDefault,
                                                 bool gpuDefaultActive,
                                                 bool resolvedGpuUsable,
                                                 const QString& gpuDefaultTier,
                                                 const QString& baseDefaultTier)
{
    if (resolvedGpuUsable) {
        if (wantGpuDefault) {
            return {gpuDefaultTier, true};
        }
        return {currentTier, gpuDefaultActive};
    }
    if (gpuDefaultActive && currentTier == gpuDefaultTier) {
        return {baseDefaultTier, false};
    }
    // Off the GPU the auto-raise state is meaningless — drop it so a stale
    // flag can never walk back a tier the operator has since chosen.
    return {currentTier, false};
}

// A selectable transcription language: `code` is the ISO code passed to the
// backend (e.g. "en", "es"); `name` is the English display name ("English").
struct AsrLanguage {
    QString code;
    QString name;
};

// Every language the vendored whisper build supports, sorted by display name.
// Multilingual models honor the choice; English-only (.en) models ignore it.
// There is no "auto-detect" entry: passing a language of "auto" (or empty) to
// whisperAsrBackendFactory still triggers detection in transcribe(), but the UI
// does not offer that option — detection was unreliable on Copy Assist's short
// VAD segments (whisper keys off ~30 s of audio), so the path is left dormant.
std::vector<AsrLanguage> asrWhisperLanguages();

// Coerce a persisted language code to one the model can actually decode:
// returns `saved` when it appears in `supported`, otherwise falls back to
// English ("en"). Used to validate/migrate the stored AsrLanguage (including
// the retired "auto" sentinel and any empty/stale code) so the selector and the
// engine can never disagree. Header-inline and whisper-free so it is unit
// testable without linking the vendored library.
inline QString asrLanguageOrDefault(const QString& saved,
                                    const std::vector<AsrLanguage>& supported)
{
    for (const AsrLanguage& lang : supported) {
        if (lang.code == saved) {
            return saved;
        }
    }
    return QStringLiteral("en");
}

} // namespace AetherSDR
