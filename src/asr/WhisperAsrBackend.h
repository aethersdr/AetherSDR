#pragma once

#include "asr/IAsrBackend.h"

#include <QString>

#include <functional>
#include <memory>
#include <vector>

struct whisper_context;

namespace AetherSDR {

// Below this confidence, don't let a segment's text seed the next decode's
// prompt — a garbled/low-confidence result is more likely to compound into a
// worse one downstream (a known whisper hallucination-propagation failure mode)
// than to help continuity. 0.65 aligns with CopyAssistPanel's "yellow" band
// (colorForConfidence): text the UI paints sub-yellow is exactly what we don't
// want to carry. Not exposed in the UI — kept deliberately simple.
inline constexpr float kContextCarryMinConfidence = 0.65f;

// Whether a decoded segment should become the next decode's carried context
// prompt (RFC #4818). Free + inline so it is unit-testable without a loaded
// whisper_context: carry only confident, non-empty text — a marginal or empty
// decode returns false, which the caller honours by leaving the previous prompt
// in place rather than replacing it with garbage.
inline bool asrShouldCarryContext(const QString& text, float confidence)
{
    return !text.isEmpty() && confidence >= kContextCarryMinConfidence;
}

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
    // Opt-in (RFC #4818): see IAsrBackend::setContextCarryEnabled. The
    // carried text is passed as an explicit decode prompt (whisper's
    // initial_prompt), and only a segment whose confidence clears the gate (see
    // .cpp) is stored as the next call's prompt, so a garbled decode can't poison
    // it.
    void setContextCarryEnabled(bool on) override;
    // Drop the carried prompt so the next decode starts clean (long silence gap
    // or explicit Clear). See IAsrBackend::resetContext.
    void resetContext() override;

private:
    whisper_context* m_ctx = nullptr;
    QString m_language;
    int m_threads = 0;
    int m_gpuDevice = 0;
    // Whether m_ctx was created on the GPU. False after the CPU retry in
    // load(), so a decode-time throw never latches a device that was not
    // involved.
    bool m_ctxOnGpu = false;
    bool m_contextCarryEnabled = false;
    // The last confident segment's text, carried forward as the next decode's
    // explicit prompt when context-carry is on. Empty = start clean (right after
    // load(), a reset, or while no segment has cleared the confidence gate).
    QString m_carriedPrompt;
};

// A selectable GPU: `index` is the value to pass as gpuDevice (its position among
// GPU/IGPU devices in ggml's enumeration order); `name` is a human description.
// `usable` is the session verdict: false when the device failed the decode
// capability probe, or when a model load on it failed earlier this run.
struct AsrGpuDevice {
    int index = 0;
    QString name;
    bool usable = true;
    // Physical memory as reported by ggml_backend_dev_memory (#4986); both 0
    // when unknown — query unavailable, or the device was latched out before
    // it could be asked.
    quint64 vramFreeBytes = 0;
    quint64 vramTotalBytes = 0;
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

// whisper.cpp and ggml report through one log callback, which by default writes
// to stderr — so nothing they say reaches the log file or a support bundle.
// AsrLibLogAssembler is the policy for routing that stream into the log file:
// WARN and ERROR are forwarded, INFO and DEBUG are dropped (a single model load
// prints dozens of INFO lines), and text arrives one callback per printf, so it
// is split into whole lines. GGML_LOG_LEVEL_CONT ("continue the previous
// message") is honoured by joining, though nothing in the vendored whisper/ggml
// emits it — multi-part messages there repeat the level instead, and so stay
// separate lines; the CONT branch is for a system libwhisper that does. Header-inline and
// whisper-free, like the helpers above, so it is unit testable without linking
// the vendored library; the level constants mirror ggml_log_level and the .cpp
// static_asserts that they still match.
//
// Not thread-safe: the caller serialises feed() (whisper logs from the ASR
// worker thread, discovery from a pool thread).
class AsrLibLogAssembler {
public:
    static constexpr int kLevelWarn = 3;  // GGML_LOG_LEVEL_WARN
    static constexpr int kLevelError = 4; // GGML_LOG_LEVEL_ERROR
    static constexpr int kLevelCont = 5;  // GGML_LOG_LEVEL_CONT

    struct Line {
        bool error = false; // false = warning
        QString text;
    };

    // Feed one callback invocation; returns the complete lines it finished that
    // are to be forwarded (usually none or one).
    std::vector<Line> feed(int level, const char* text)
    {
        std::vector<Line> out;
        if (text == nullptr) {
            return out;
        }
        if (level != kLevelCont) {
            // A new message. whisper and ggml end every message with '\n', but
            // do not depend on it: finish whatever the last one left open.
            finishPending(out);
            m_forward = (level == kLevelWarn || level == kLevelError);
            m_error = (level == kLevelError);
        }
        if (!m_forward) {
            return out;
        }
        m_pending += QString::fromUtf8(text);
        qsizetype newline = -1;
        while ((newline = m_pending.indexOf(QLatin1Char('\n'))) >= 0) {
            emitLine(m_pending.left(newline), out);
            m_pending.remove(0, newline + 1);
        }
        if (m_pending.size() > kMaxPendingChars) {
            finishPending(out); // never grow without bound on a newline-free stream
        }
        return out;
    }

private:
    static constexpr int kMaxPendingChars = 4096;

    void emitLine(const QString& raw, std::vector<Line>& out) const
    {
        const QString line = raw.trimmed();
        if (!line.isEmpty()) {
            out.push_back({m_error, line});
        }
    }

    void finishPending(std::vector<Line>& out)
    {
        if (m_forward) {
            emitLine(m_pending, out);
        }
        m_pending.clear();
    }

    bool m_forward = false;
    bool m_error = false;
    QString m_pending;
};

// Route whisper/ggml WARN + ERROR into the log file (category
// aether.asr.whisper) while leaving their stderr output as it was (see the
// callback for the one subtlety: DEBUG lines stop after the first model load,
// as they did before).
// Called by the application, not from inside this library: a test or tool that
// wants ggml's log for itself (asr_gpu_probe_test does) must keep it. Forwarded
// lines are flushed to disk only while an AsrStageTrace is open — the window in
// which a crash would otherwise lose them; decode-path warnings ride the
// writer's normal timer. NOT reached by this: ggml-vulkan reports most of its
// failures on std::cerr directly (e.g. "Device memory allocation of size N
// failed"), which no log callback sees.
void asrInstallLogRouting();

// Whether a model tier of `tierSizeBytes` (the weights file) can be expected to
// load on a device reporting this much memory. Gates only the AUTOMATIC raise
// to the GPU-default tier: "a GPU exists" says nothing about room, and a 1.6 GB
// model auto-selected for a 2 GB card is #4972. An explicit operator choice is
// never refused here — that stays the operator's call.
//
// The headroom is what whisper allocates beyond the weights (KV caches and
// compute buffers). MEASURED (#4972 bench, RTX 5060 Laptop, ggml-vulkan,
// 2026-09-16): large-v3-turbo occupies 1818 MiB against a 1549 MiB file
// (+268 MiB), base 293 MiB against 141 MiB (+152 MiB); whisper's own load log
// sums to the same figure. 300 MiB covers the larger of the two.
//
// The free figure is not always free memory: ggml-vulkan reports free == total
// for a device without VK_EXT_memory_budget (ggml_backend_vk_get_device_memory),
// so the free check alone can be handed the whole heap. The total must therefore
// clear the same need plus a reserve for the desktop and AetherSDR's own
// rendering on that card, which makes the answer independent of the reporting
// mode. The reserve is a chosen margin, not a measurement; for scale, MEASURED
// total minus free at the startup probe was 367 MiB (#5730 reporter log, GTX
// 1050, 1809 of 2176 MB free) and 791 MiB (#4972 bench, 7360 of 8151 MB free).
//
// Both figures 0 means the device could not be asked (AsrGpuDevice) — unknown
// is not "too small", so it keeps the previous behaviour. Integrated GPUs
// report shared system memory and pass on their own numbers. Header-inline and
// whisper-free, like asrReconcileDefaultTier above.
inline constexpr quint64 kAsrTierVramHeadroomBytes = 300ull * 1024ull * 1024ull;
inline constexpr quint64 kAsrTierVramDesktopReserveBytes = 512ull * 1024ull * 1024ull;

inline bool asrTierFitsVram(quint64 vramFreeBytes, quint64 vramTotalBytes, qint64 tierSizeBytes)
{
    if (vramTotalBytes == 0 || tierSizeBytes <= 0) {
        return true;
    }
    const quint64 need = static_cast<quint64>(tierSizeBytes) + kAsrTierVramHeadroomBytes;
    return vramFreeBytes >= need && vramTotalBytes >= need + kAsrTierVramDesktopReserveBytes;
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
