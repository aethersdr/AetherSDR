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

// After device resolution, which tier should run. Raise to the GPU-default tier
// only when no explicit operator choice exists and a usable GPU was resolved;
// walk an auto-raised tier (`gpuDefaultActive`) back to the base default when
// resolution falls off the GPU (it can't keep up on CPU, #4502); never touch an
// operator-picked tier. Whisper-free for unit tests.
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

// Routes the whisper/ggml log callback (stderr by default) into the log file:
// WARN/ERROR forwarded, INFO/DEBUG dropped, printf-sized chunks joined into whole
// lines, GGML_LOG_LEVEL_CONT joined to the previous message (for a system
// libwhisper; vendored code never emits it). Level constants mirror
// ggml_log_level, static_asserted in the .cpp. Not thread-safe: caller serialises
// feed().
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

// Route whisper/ggml WARN + ERROR into aether.asr.whisper, leaving stderr output
// unchanged. Called by the application only, so tests (asr_gpu_probe_test) keep
// ggml's log. Lines are flushed synchronously only while an AsrStageTrace is
// open. ggml-vulkan's direct std::cerr output is not captured.
void asrInstallLogRouting();

// Whether a tier of `tierSizeBytes` should load on a device with this much memory.
// Gates only the AUTOMATIC raise to the GPU-default tier (#4972); an explicit
// operator choice is never refused. Headroom = whisper's KV caches + compute
// buffers: measured large-v3-turbo +268 MiB, base +152 MiB (RTX 5060, ggml-vulkan),
// so 300 MiB. ggml-vulkan reports free == total without VK_EXT_memory_budget, so
// total must also clear the need plus a chosen 512 MiB desktop reserve (observed
// total-free: 367 and 791 MiB). Both figures 0 = unknown, allowed.
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
