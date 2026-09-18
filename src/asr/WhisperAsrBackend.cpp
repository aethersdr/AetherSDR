#include "asr/WhisperAsrBackend.h"

#include <QFileInfo>
#include <QLoggingCategory>
#include <QThread>

#include <algorithm>
#include <exception>
#include <mutex>
#include <set>

#include <ggml-backend.h>
#include <whisper.h>

#ifdef Q_OS_MACOS
#include <sys/sysctl.h>
#endif

namespace AetherSDR {

Q_LOGGING_CATEGORY(lcAsrWhisper, "aether.asr.whisper")

namespace {
// Whisper is chunk-based; on HF audio a single over is well under 30 s, so we
// cap inference threads modestly to leave the box responsive (esp. on a Pi).
int chooseThreadCount()
{
    const int hw = QThread::idealThreadCount();
    if (hw <= 0) {
        return 4;
    }
    return std::clamp(hw, 1, 4);
}

// Whether this host may enumerate Metal at all.
//
// The hazard being avoided is #4535: Apple's *runtime* shader compiler
// (newLibraryWithSource) can live-lock on Intel-GPU Macs — measured at no
// completion in 75 minutes on a Radeon Pro 560X — and ggml reaches it from
// plain device enumeration. So on a build that embeds the shader SOURCE, Intel
// Macs must not enumerate Metal: the first ggml touch would hang the caller.
//
// A build that embeds a precompiled .metallib (AETHER_ASR_METAL_PRECOMPILED,
// the default and what every release ships) cannot reach that compiler at all,
// so the gate is not needed and is not applied — an Intel Mac gets whatever
// Metal device ggml enumerates, and ggml's own per-op capability checks decide
// what actually runs on it. Keeping the gate on that build would withdraw the
// GPU from Metal3-class AMD hardware (Vega II, W5700X, 5700XT) purely on
// vendor, which is a policy nobody measured.
//
// Checked via hardware sysctl rather than build arch so an x86_64 build under
// Rosetta still sees the real GPU. AETHER_ASR_FORCE_METAL=1 overrides, for
// diagnostics.
bool asrMetalUsableHost()
{
#if defined(Q_OS_MACOS) && !defined(AETHER_ASR_METAL_PRECOMPILED)
    int isArm64 = 0;
    size_t size = sizeof(isArm64);
    if (sysctlbyname("hw.optional.arm64", &isArm64, &size, nullptr, 0) != 0) {
        isArm64 = 0; // key absent = pre-Apple-Silicon macOS
    }
    if (isArm64 == 1) {
        return true;
    }
    // Test the override only once the host is known NOT to be Apple Silicon, so
    // the log line describes what actually happened. (Checking it first made an
    // Apple Silicon run with the variable set announce a "non-Apple-Silicon
    // host".)
    if (qEnvironmentVariableIsSet("AETHER_ASR_FORCE_METAL")) {
        static const bool logged = [] {
            qCInfo(lcAsrWhisper) << "AETHER_ASR_FORCE_METAL set - offering Metal despite non-Apple-Silicon host";
            return true;
        }();
        Q_UNUSED(logged)
        return true;
    }
    static const bool logged = [] {
        qCInfo(lcAsrWhisper) << "Intel Mac on a source-embed build - ASR is CPU-only "
                                "(Metal not offered; rebuild with the offline Metal "
                                "toolchain to enable it)";
        return true;
    }();
    Q_UNUSED(logged)
    return false;
#else
    return true;
#endif
}

// whisper_init_from_file_with_params() reaches real GPU device and context
// creation inside ggml (Vulkan on Linux/Windows, Metal on Apple Silicon). A
// broken driver stack there surfaces as a thrown exception — e.g. vulkan-hpp's
// vk::SystemError escaping ggml_vk_instance_init()/ggml_vk_get_device(), which
// unlike ggml_backend_vk_reg() have no internal guard. Unguarded, that
// exception unwinds out of the ASR worker's slot and terminates the whole app
// (#4502). Catch it and report through the backend's error path, mirroring the
// ONNX backends (SileroVad/SpeakerEmbedder). ggml's GGML_ABORT paths call
// abort() and cannot be caught; this covers the throwing half only.
whisper_context* initWhisperContext(const QByteArray& pathUtf8,
                                    const whisper_context_params& cparams,
                                    QString* failure)
{
    try {
        return whisper_init_from_file_with_params(pathUtf8.constData(), cparams);
    } catch (const std::exception& e) {
        if (failure != nullptr) {
            *failure = QString::fromUtf8(e.what());
        }
    } catch (...) {
        if (failure != nullptr) {
            *failure = QStringLiteral("unknown exception");
        }
    }
    return nullptr;
}
} // namespace

WhisperAsrBackend::WhisperAsrBackend()
    : WhisperAsrBackend(QStringLiteral("en"))
{
}

WhisperAsrBackend::WhisperAsrBackend(QString language, int gpuDevice)
    : m_language(std::move(language))
    , m_threads(chooseThreadCount())
    , m_gpuDevice(gpuDevice)
{
}

WhisperAsrBackend::~WhisperAsrBackend()
{
    unload();
}

bool WhisperAsrBackend::load(const QString& modelPath, QString* error)
{
    unload();

    if (!QFileInfo::exists(modelPath)) {
        if (error != nullptr) {
            *error = QStringLiteral("model file not found: %1").arg(modelPath);
        }
        return false;
    }

    whisper_context_params cparams = whisper_context_default_params();
    // gpu_device selects the GPU (index among GPU devices; see asrGpuDevices), or
    // -1 to force CPU. Otherwise use a GPU backend (Vulkan/Metal) when one is
    // compiled in and present; ggml falls back to CPU automatically when not.
    // A device that failed a load earlier this session is never re-entered:
    // ggml's GPU instance state is sticky, and a second attempt against it can
    // fault below the level any caller can guard (#4502).
    const bool useGpu =
        m_gpuDevice >= 0 && asrGpuAvailable() && !asrGpuDeviceFailed(m_gpuDevice);
    cparams.use_gpu = useGpu;
    cparams.gpu_device = useGpu ? m_gpuDevice : 0;

    const QByteArray pathUtf8 = modelPath.toUtf8();
    QString failure;
    m_ctx = initWhisperContext(pathUtf8, cparams, &failure);
    if (m_ctx == nullptr && useGpu) {
        // A GPU that enumerates can still be unusable at load time, and
        // ggml-vulkan's instance state is sticky per process — a second GPU
        // attempt re-enters the half-built state (the fail-then-crash pattern
        // of #4502). Retry once on CPU, which never touches the GPU backend.
        //
        // Only blame the GPU when the model itself is sound: an empty or
        // unreadable file yields the same null context, and latching the
        // device for that reports broken hardware for a bad download — and
        // costs the GPU until restart, since the latch is one-way. The latch
        // itself stays wide (`failure` is empty in exactly the dangerous
        // case: whisper's internal catch swallows the vk::SystemError and
        // returns null, so there is no discriminator on the exception side),
        // and it stays BEFORE the CPU retry so a retry that fails for its own
        // reasons — say, out of memory — cannot leave a poisoned device
        // unlatched.
        const QFileInfo modelInfo(modelPath);
        const bool modelPlausible = modelInfo.isReadable() && modelInfo.size() > 0;
        qCWarning(lcAsrWhisper) << "GPU model load failed"
                                << (failure.isEmpty() ? QStringLiteral("(null context)") : failure)
                                << "- retrying on CPU";
        if (modelPlausible) {
            asrMarkGpuDeviceFailed(m_gpuDevice);
        } else {
            qCWarning(lcAsrWhisper)
                << "model file is empty or unreadable - not latching device"
                << m_gpuDevice << "; the file is the fault, not the GPU";
        }
        cparams.use_gpu = false;
        cparams.gpu_device = 0;
        // Clear the GPU attempt's message first: initWhisperContext() writes
        // `failure` only when it catches, so a CPU retry that simply returns
        // null would otherwise be reported with the GPU's Vulkan exception
        // text — blaming the driver for, say, a truncated model file.
        failure.clear();
        m_ctx = initWhisperContext(pathUtf8, cparams, &failure);
    }
    if (m_ctx == nullptr) {
        if (error != nullptr) {
            *error = failure.isEmpty()
                ? QStringLiteral("whisper failed to load model: %1").arg(modelPath)
                : QStringLiteral("whisper failed to load model: %1 (%2)").arg(modelPath, failure);
        }
        return false;
    }

    // Whether a GPU was asked for on the attempt that actually produced this
    // context: false after the CPU retry above, even though m_gpuDevice still
    // names the GPU. transcribe() consults it so a decode-time throw latches
    // the device only when the GPU was in the picture. It records the request,
    // not proof of placement — whisper falls back to CPU internally if
    // whisper_backend_init_gpu() yields nothing — so the flag errs toward
    // latching. That is the safe direction: an over-latch costs a session of
    // GPU speed, an under-latch costs the process (#4502).
    m_ctxOnGpu = cparams.use_gpu;

    qCInfo(lcAsrWhisper) << "Loaded model" << modelPath << "(" << m_threads << "threads )";
    return true;
}

AsrTranscript WhisperAsrBackend::transcribe(const std::vector<float>& pcm16k, QString* error)
{
    if (m_ctx == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("no model loaded");
        }
        return {};
    }
    if (m_ctxOnGpu && asrGpuDeviceFailed(m_gpuDevice)) {
        // The device this context lives on has been latched since the load —
        // decoding on it again is exactly the second attempt the session
        // contract forbids (#4502), and without this guard it stays possible
        // in the window between the latch and the queued engine rebuild that
        // retires this context. Refuse instead; the error keeps nudging the
        // GUI's rebuild path, whose reconcile entry guard makes repeats
        // harmless.
        if (error != nullptr) {
            *error = QStringLiteral("GPU device is latched out after a failure; "
                                    "decode refused until the engine rebuilds off it");
        }
        return {};
    }
    if (pcm16k.empty()) {
        return {};
    }

    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.n_threads = m_threads;
    wparams.translate = false;
    // Never reuse whisper's INTERNAL cross-call prompt (prompt_past): it is
    // rebuilt from every decode's output unconditionally, so the confidence gate
    // can't reach it — that was the "kept re-seeding and couldn't recover" bug.
    // Instead we condition explicitly via initial_prompt below, which the gate
    // and resetContext() fully control.
    wparams.no_context = true;
    wparams.single_segment = true; // one utterance -> one segment
    wparams.no_timestamps = true;
    wparams.print_progress = false;
    wparams.print_realtime = false;
    wparams.print_timestamps = false;
    wparams.print_special = false;
    wparams.suppress_blank = true;

    // Context carry (opt-in): condition this decode on the last confident
    // segment's text by passing it as an explicit prompt. promptUtf8 must outlive
    // whisper_full() (it holds the buffer initial_prompt points at); whisper
    // tokenizes it synchronously during the call.
    QByteArray promptUtf8;
    if (m_contextCarryEnabled && !m_carriedPrompt.isEmpty()) {
        promptUtf8 = m_carriedPrompt.toUtf8();
        wparams.initial_prompt = promptUtf8.constData();
    }

    // "auto" (or empty) → let whisper detect the language from the audio and
    // then transcribe it; any other value pins decoding to that language.
    // whisper_full() treats a language of "auto" as detect-then-transcribe on
    // its own. detect_language MUST stay false: when it is true whisper detects
    // the language and returns early WITHOUT transcribing (whisper.cpp:
    // `if (params.detect_language) return 0;`), which would yield empty output.
    const QByteArray langUtf8 = m_language.toUtf8();
    const bool autoDetect = langUtf8.isEmpty() || langUtf8 == "auto";
    wparams.language = autoDetect ? "auto" : langUtf8.constData();
    wparams.detect_language = false;

    // Same guard as the load path: inference reaches the GPU backend too, and
    // an exception escaping the worker thread would terminate the app. A GPU
    // that loaded the model fine can still fail here — and the session contract
    // is the same as for a failed load: never hand this device another attempt
    // (a repeat touch of the now-poisoned backend state is the class of #4502).
    // Latch only when the context actually lives on the GPU; a CPU-side throw
    // (e.g. an allocation failure) says nothing about the GPU. The GUI rebuilds
    // the engine off the failed device when the error signal arrives.
    int status = -1;
    try {
        status = whisper_full(m_ctx, wparams, pcm16k.data(), static_cast<int>(pcm16k.size()));
    } catch (const std::exception& e) {
        if (m_ctxOnGpu) {
            asrMarkGpuDeviceFailed(m_gpuDevice);
        }
        if (error != nullptr) {
            *error = QStringLiteral("whisper_full() failed: %1").arg(QString::fromUtf8(e.what()));
        }
        return {};
    } catch (...) {
        if (m_ctxOnGpu) {
            asrMarkGpuDeviceFailed(m_gpuDevice);
        }
        if (error != nullptr) {
            *error = QStringLiteral("whisper_full() failed: unknown exception");
        }
        return {};
    }
    if (status != 0) {
        if (error != nullptr) {
            *error = QStringLiteral("whisper_full() failed");
        }
        return {};
    }

    // Special tokens (timestamps, <eot>, etc.) have ids >= the end-of-text token;
    // they're excluded from the confidence mean below so punctuation/formatting
    // doesn't skew the score.
    const whisper_token specialFloor = whisper_token_eot(m_ctx);

    QString text;
    double probSum = 0.0;
    int probCount = 0;
    const int segments = whisper_full_n_segments(m_ctx);
    for (int i = 0; i < segments; ++i) {
        // Text via whisper's own byte-safe assembly — NOT rebuilt token-by-token:
        // whisper can split one multi-byte UTF-8 character across token
        // boundaries (BPE / byte-fallback), and QString::fromUtf8 on each partial
        // fragment would yield U+FFFD, corrupting non-ASCII text (#4399 languages).
        const char* seg = whisper_full_get_segment_text(m_ctx, i);
        if (seg != nullptr) {
            text += QString::fromUtf8(seg);
        }
        // Confidence = mean probability over the real (non-special) tokens.
        const int nTokens = whisper_full_n_tokens(m_ctx, i);
        for (int t = 0; t < nTokens; ++t) {
            if (whisper_full_get_token_id(m_ctx, i, t) >= specialFloor) {
                continue;
            }
            probSum += whisper_full_get_token_p(m_ctx, i, t);
            ++probCount;
        }
    }

    AsrTranscript result;
    result.text = text.trimmed();
    result.confidence = probCount > 0 ? static_cast<float>(probSum / probCount) : 0.0f;

    // Only a confident segment becomes the next decode's prompt; a marginal one
    // leaves the last good prompt in place rather than replacing it with garbage.
    // (A real pause or Clear drops it entirely via resetContext().)
    if (m_contextCarryEnabled && asrShouldCarryContext(result.text, result.confidence)) {
        // whisper consumes at most the LAST min(n_max_text_ctx, n_text_ctx/2)-1
        // ≈ 223 prompt tokens (whisper.cpp max_prompt_ctx), and a decode can echo
        // initial_prompt back into its own text — so keep only the tail, or an
        // echo would compound the carried string run over run.
        constexpr int kMaxCarriedChars = 1000;
        m_carriedPrompt = result.text.right(kMaxCarriedChars);
    }

    return result;
}

void WhisperAsrBackend::setContextCarryEnabled(bool on)
{
    m_contextCarryEnabled = on;
    if (!on) {
        m_carriedPrompt.clear(); // start clean when re-enabled
    }
}

void WhisperAsrBackend::resetContext()
{
    // Drop the carried prompt so the next decode starts clean. Lets the engine
    // recover after a long silence or an explicit Clear.
    m_carriedPrompt.clear();
}

void WhisperAsrBackend::unload()
{
    if (m_ctx != nullptr) {
        whisper_free(m_ctx);
        m_ctx = nullptr;
    }
    m_ctxOnGpu = false;
    // A new/different model has no relationship to whatever text the old one
    // produced — don't carry a stale prompt into its first decode.
    m_carriedPrompt.clear();
}

std::function<std::unique_ptr<IAsrBackend>()> whisperAsrBackendFactory(const QString& language,
                                                                       int gpuDevice)
{
    return [language, gpuDevice]() -> std::unique_ptr<IAsrBackend> {
        return std::make_unique<WhisperAsrBackend>(language, gpuDevice);
    };
}

namespace {

// Devices whose model load failed this run. Whisper loads happen on the ASR
// worker thread while enumeration runs on a QtConcurrent pool thread, so the
// set is mutex-guarded. Never cleared: see asrMarkGpuDeviceFailed's contract.
std::mutex& asrFailedGpuMutex()
{
    static std::mutex m;
    return m;
}

std::set<int>& asrFailedGpuDevices()
{
    static std::set<int> failed;
    return failed;
}

// Whether `dev` can run the kernels whisper's heavy path schedules on the GPU.
// Probed through the public ggml_backend_dev_supports_op with synthetic ops — a
// contiguous F32 soft-max, an F16 mat-mul, and a flash-attention op (whisper
// enables flash attention unconditionally) — rather than by reading
// backend-internal device properties, so one probe serves every GPU backend.
// The op shapes model the decode of the vendored whisper.cpp (1.9.1 at this
// writing): head dim 64, F16 K/V, unconditional flash attention. They are a
// prediction, not a contract — a whisper.cpp bump that changes any of that
// quietly changes what this probe should be asking, so re-derive the shapes
// whenever the vendored copy moves.
// supports_op only consults device properties: nothing is compiled or allocated
// (no_alloc context, freed before returning).
//
// On the Vulkan backends this is deliberately the FIRST touch of the device:
// ggml_backend_vk_device_supports_op resolves ggml_vk_get_device, which creates
// the logical device. A driver stack that cannot create one fails here — once,
// early, and survivably — instead of at model-load time, where a repeat attempt
// against ggml's already-initialised instance state is no longer recoverable.
bool asrDeviceUsableForDecode(ggml_backend_dev_t dev)
{
    ggml_init_params params = {};
    params.mem_size = 16 * 1024;
    params.no_alloc = true;
    ggml_context* ctx = ggml_init(params);
    if (ctx == nullptr) {
        return false;
    }
    // supports_op can throw on a hostile Vulkan stack — that path is caught
    // (and latched) by the caller, so the context must free on it too, not
    // only on the straight-line return.
    struct CtxFree {
        ggml_context* c;
        ~CtxFree() { ggml_free(c); }
    } ctxFree{ctx};
    ggml_tensor* act = ggml_new_tensor_2d(ctx, GGML_TYPE_F32, 64, 64);
    ggml_tensor* softMax = ggml_soft_max(ctx, act);
    ggml_tensor* weights = ggml_new_tensor_2d(ctx, GGML_TYPE_F16, 64, 64);
    ggml_tensor* mulMat = ggml_mul_mat(ctx, weights, act);
    // Head dim 64 + F16 K/V mirrors whisper's encoder.
    ggml_tensor* q = ggml_new_tensor_3d(ctx, GGML_TYPE_F32, 64, 4, 8);
    ggml_tensor* k = ggml_new_tensor_3d(ctx, GGML_TYPE_F16, 64, 4, 8);
    ggml_tensor* v = ggml_new_tensor_3d(ctx, GGML_TYPE_F16, 64, 4, 8);
    ggml_tensor* flashAttn =
        ggml_flash_attn_ext(ctx, q, k, v, nullptr, 1.0f, 0.0f, 0.0f);
    return ggml_backend_dev_supports_op(dev, softMax)
        && ggml_backend_dev_supports_op(dev, mulMat)
        && ggml_backend_dev_supports_op(dev, flashAttn);
}

} // namespace

void asrMarkGpuDeviceFailed(int index)
{
    if (index < 0) {
        return; // CPU is the fallback; it is never latched out
    }
    const std::lock_guard<std::mutex> lock(asrFailedGpuMutex());
    if (asrFailedGpuDevices().insert(index).second) {
        // Reached from both the probe and the model load, so the reason is not
        // named here — the caller logs it.
        qCWarning(lcAsrWhisper)
            << "GPU device" << index
            << "marked unusable for the rest of this session; it will not be tried again";
    }
}

bool asrGpuDeviceFailed(int index)
{
    if (index < 0) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(asrFailedGpuMutex());
    return asrFailedGpuDevices().count(index) > 0;
}

int asrResolveDefaultGpuIndex(const std::vector<AsrGpuDevice>& devices)
{
    for (const AsrGpuDevice& d : devices) {
        if (d.usable) {
            return d.index;
        }
    }
    return -1; // CPU
}

std::vector<AsrGpuDevice> asrGpuDevices()
{
    if (!asrMetalUsableHost()) {
        return {};
    }

    // Enumerate GPU + integrated-GPU devices in the same order whisper's
    // gpu_device indexes them (see whisper_backend_init_gpu).
    //
    // The first call in the process constructs ggml's backend registry, and
    // the Vulkan per-device queries sit beyond ggml_backend_vk_reg()'s
    // internal guard — on a hostile driver stack they throw (#4509 analysis),
    // which from here would terminate the process. Degrade to "no devices"
    // (CPU-only) instead; a partial enumeration is discarded rather than
    // returned, so an index never points at a device other than the one
    // whisper would pick.
    std::vector<AsrGpuDevice> devices;
    try {
        int index = 0;
        for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
            ggml_backend_dev_t dev = ggml_backend_dev_get(i);
            const auto type = ggml_backend_dev_type(dev);
            if (type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU) {
                AsrGpuDevice d;
                d.index = index++;
                d.name = QString::fromUtf8(ggml_backend_dev_description(dev));
                // A device that already failed a load this session stays
                // unusable no matter what the probe would now say — and is not
                // re-probed, since probing is itself a device-creation attempt.
                if (asrGpuDeviceFailed(d.index)) {
                    d.usable = false;
                } else {
                    // Probe each device inside its own guard: on the Vulkan
                    // backends this creates the logical device, so a broken
                    // driver throws here. That verdict belongs to this device
                    // alone — a second, healthy GPU in the same box must stay
                    // usable. The device is kept in the list either way, so
                    // the indices whisper's gpu_device refers to never shift.
                    try {
                        // Physical-memory query first (#4986): device
                        // properties only, useful even for a device that then
                        // fails the probe. A throw here lands in the same
                        // latch handlers as a probe throw — a device that
                        // cannot answer a property query is not one to retry.
                        size_t vramFree = 0, vramTotal = 0;
                        ggml_backend_dev_memory(dev, &vramFree, &vramTotal);
                        d.vramFreeBytes = vramFree;
                        d.vramTotalBytes = vramTotal;
                        d.usable = asrDeviceUsableForDecode(dev);
                        if (!d.usable) {
                            qCInfo(lcAsrWhisper)
                                << "GPU device" << d.index << d.name
                                << "fails the decode capability probe - not offered as default";
                        }
                    } catch (const std::exception& e) {
                        // A THROW here means device creation itself failed, so
                        // ggml's instance state is now half-initialised for
                        // this device. Latch it out: a later attempt — an
                        // explicit pick, or a restored preference — would be
                        // the second creation attempt, which can fault below
                        // any guard (#4502). A probe that merely returns false
                        // is not latched: that device was created fine, it
                        // just cannot run the decode, and staying selectable
                        // for diagnostics is harmless.
                        d.usable = false;
                        asrMarkGpuDeviceFailed(d.index);
                        qCWarning(lcAsrWhisper)
                            << "GPU device" << d.index << d.name
                            << "threw during the memory query or capability probe:" << e.what()
                            << "- not offered, and not retried this session";
                    } catch (...) {
                        d.usable = false;
                        asrMarkGpuDeviceFailed(d.index);
                        qCWarning(lcAsrWhisper)
                            << "GPU device" << d.index << d.name
                            << "threw during the memory query or capability probe (unknown exception)"
                            << "- not offered, and not retried this session";
                    }
                }
                // One inventory line per device — name, VRAM, verdict — so a
                // support log answers "which GPU, how much memory" without a
                // follow-up ask (#4986).
                if (d.vramTotalBytes > 0) {
                    qCInfo(lcAsrWhisper)
                        << "GPU device" << d.index << d.name
                        << "- VRAM free" << (d.vramFreeBytes / (1024 * 1024))
                        << "of" << (d.vramTotalBytes / (1024 * 1024)) << "MB -"
                        << (d.usable ? "usable" : "not offered");
                } else {
                    qCInfo(lcAsrWhisper)
                        << "GPU device" << d.index << d.name
                        << "- VRAM unknown -"
                        << (d.usable ? "usable" : "not offered");
                }
                devices.push_back(std::move(d));
            }
        }
    } catch (const std::exception& e) {
        qCWarning(lcAsrWhisper) << "GPU device enumeration failed:" << e.what();
        devices.clear();
    } catch (...) {
        qCWarning(lcAsrWhisper) << "GPU device enumeration failed: unknown exception";
        devices.clear();
    }
    return devices;
}

bool asrGpuAvailable()
{
    // True when ggml has at least one GPU (discrete or integrated) device.
    return !asrGpuDevices().empty();
}

std::vector<AsrLanguage> asrWhisperLanguages()
{
    // Enumerate every language the vendored whisper build supports, straight
    // from its own table (whisper_lang_str = ISO code, whisper_lang_str_full =
    // English name), so this never drifts out of sync with the model. The
    // multilingual checkpoints cover them all; the .en-only models ignore the
    // setting and always decode English. Sorted by display name. There is no
    // "auto-detect" entry — whisper's table has no such code, and the UI does
    // not add one (detection was unreliable on short VAD segments).
    std::vector<AsrLanguage> langs;
    const int maxId = whisper_lang_max_id();
    langs.reserve(static_cast<size_t>(maxId) + 1);
    for (int id = 0; id <= maxId; ++id) {
        const char* code = whisper_lang_str(id);
        const char* full = whisper_lang_str_full(id);
        if (code == nullptr || full == nullptr) {
            continue;
        }
        AsrLanguage lang;
        lang.code = QString::fromUtf8(code);
        // whisper's full names are lowercase ("english", "haitian creole");
        // title-case each word for display ("Haitian Creole") without pulling in
        // a locale.
        QString name = QString::fromUtf8(full);
        bool startOfWord = true;
        for (QChar& ch : name) {
            if (ch.isSpace()) {
                startOfWord = true;
            } else if (startOfWord && ch.isLetter()) {
                ch = ch.toUpper();
                startOfWord = false;
            }
        }
        lang.name = name;
        langs.push_back(std::move(lang));
    }
    std::sort(langs.begin(), langs.end(),
              [](const AsrLanguage& a, const AsrLanguage& b) { return a.name < b.name; });
    return langs;
}

} // namespace AetherSDR
