#ifdef HAVE_NVIDIA_AFX

#include "NvidiaAfxFilter.h"
#include "Resampler.h"

#include <QCoreApplication>
#include <QDir>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QStringList>

Q_LOGGING_CATEGORY(lcNvAfx, "aether.nvafx")

#if defined(_WIN32)
// windows.h's min/max function-like macros otherwise clobber std::min and
// std::numeric_limits<>::max() at their use sites (MSVC error C2589).
#  ifndef NOMINMAX
#  define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif
#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

namespace AetherSDR {

// ─── Cross-platform dynamic-load primitives ──────────────────────────────────
// Linux: dlopen/dlsym/dlclose. Windows: LoadLibrary/GetProcAddress/FreeLibrary.
// Handles are stored as void* (an HMODULE is a pointer-width handle).
namespace {

#if defined(_WIN32)
void* afxLoadLibrary(const QString& path)
{
    // LOAD_WITH_ALTERED_SEARCH_PATH makes the directory containing the loaded
    // DLL the first place its own dependencies (CUDA/TensorRT siblings in the
    // pack's bin dir) are resolved from — the Windows analogue of RPATH.
    return ::LoadLibraryExW(reinterpret_cast<const wchar_t*>(path.utf16()),
                            nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}
void* afxGetSymbol(void* h, const char* name)
{
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(h), name));
}
void afxCloseLibrary(void* h) { ::FreeLibrary(static_cast<HMODULE>(h)); }
QString afxLoadError()
{
    const DWORD e = ::GetLastError();
    return QStringLiteral("LoadLibrary error %1").arg(e);
}
#else
void* afxLoadLibrary(const QString& path)
{
    return ::dlopen(path.toLocal8Bit().constData(), RTLD_NOW | RTLD_GLOBAL);
}
void* afxGetSymbol(void* h, const char* name) { return ::dlsym(h, name); }
void afxCloseLibrary(void* h) { ::dlclose(h); }
QString afxLoadError()
{
    const char* e = ::dlerror();
    return QString::fromLocal8Bit(e ? e : "unknown");
}
#endif

} // namespace

// ─── Minimal NVIDIA AFX C API (dlopen'd — we do NOT vendor NVIDIA's header) ───
// Signatures mirror nvAudioEffects.h. Status 0 == success.
namespace {

using NvAFX_Handle = void*;

using fn_CreateEffect  = int (*)(const char* code, NvAFX_Handle* effect);
using fn_DestroyEffect = int (*)(NvAFX_Handle effect);
using fn_SetU32        = int (*)(NvAFX_Handle, const char* param, unsigned int val);
using fn_SetString     = int (*)(NvAFX_Handle, const char* param, const char* val);
using fn_SetFloat      = int (*)(NvAFX_Handle, const char* param, float val);
using fn_GetU32        = int (*)(NvAFX_Handle, const char* param, unsigned int* val);
using fn_Load          = int (*)(NvAFX_Handle);
using fn_Run           = int (*)(NvAFX_Handle, const float** in, float** out,
                                 unsigned num_samples, unsigned num_channels);
// SDK 1.x declared NvAFX_Reset(effect); later SDKs add a per-stream reset
// list. Called with the list form: a one-argument implementation ignores the
// trailing arguments under both the SysV x86-64 and Win64 conventions.
using fn_Reset         = int (*)(NvAFX_Handle, bool* reset_list, unsigned list_length);
using fn_InitLogger    = int (*)(int level, int target, const char* file,
                                 void* cb, void* userdata);

constexpr int    NVAFX_OK                = 0;
constexpr char   EFFECT_DENOISER[]       = "denoiser";
constexpr char   P_SAMPLE_RATE[]         = "input_sample_rate";
constexpr char   P_SAMPLE_RATE_LEGACY[]  = "sample_rate";
constexpr char   P_MODEL_PATH[]          = "model_path";
constexpr char   P_NUM_STREAMS[]         = "num_streams";
constexpr char   P_FRAME[]               = "num_samples_per_frame";
constexpr char   P_FRAME_IN[]            = "num_samples_per_input_frame";
constexpr char   P_FRAME_OUT[]           = "num_samples_per_output_frame";
constexpr char   P_INTENSITY[]           = "intensity_ratio";
constexpr char   P_USE_DEFAULT_GPU[]     = "use_default_gpu";

constexpr unsigned kSampleRate = 48000;   // AFX denoiser runs at 48 kHz here
constexpr int      kRequestedFrame = 480; // standard 48 kHz denoiser frame

} // namespace

struct NvidiaAfxFilter::Api {
    fn_CreateEffect  CreateEffect{nullptr};
    fn_DestroyEffect DestroyEffect{nullptr};
    fn_SetU32        SetU32{nullptr};
    fn_SetString     SetString{nullptr};
    fn_SetFloat      SetFloat{nullptr};
    fn_GetU32        GetU32{nullptr};
    fn_Load          Load{nullptr};
    fn_Run           Run{nullptr};
    fn_Reset         Reset{nullptr};
};

// ─── Pack resolution ────────────────────────────────────────────────────────
static QString resolvePackDir(const QString& explicitDir)
{
    if (!explicitDir.isEmpty())
        return explicitDir;
    const QByteArray env = qgetenv("AETHER_NVAFX_DIR");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);
    // Default cache location the downloader populates. NOTE: this must stay in
    // sync with NvidiaAfxPack::cacheRoot() (kept separate so the hardware test,
    // which links only this filter, doesn't drag in the whole pack + its
    // Network/Concurrent deps).
    QString data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (data.isEmpty())
        data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QDir(data).filePath(QStringLiteral("nvidia-afx/current"));
}

// Find the per-arch denoiser model: features/denoiser/models/sm_XX/denoiser_48k.trtpkg
static QString findModel(const QString& packDir)
{
    const QString modelsRoot =
        QDir(packDir).filePath(QStringLiteral("features/denoiser/models"));
    const QDir root(modelsRoot);
    if (!root.exists())
        return {};
    const QStringList smDirs = root.entryList({QStringLiteral("sm_*")},
                                              QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& sm : smDirs) {
        const QString p = root.filePath(sm + QStringLiteral("/denoiser_48k.trtpkg"));
        if (QFile::exists(p))
            return p;
    }
    return {};
}

// ─── Lifecycle ──────────────────────────────────────────────────────────────
NvidiaAfxFilter::NvidiaAfxFilter(const QString& packDir, int sampleRate)
    : m_sampleRate(sampleRate)
    , m_api(std::make_unique<Api>())
{
    if (sampleRate != 24000 && sampleRate != 48000) {
        m_lastError = QStringLiteral("Unsupported sample rate: %1").arg(sampleRate);
        return;
    }
    createResamplers();
    const QString dir = resolvePackDir(packDir);
    if (!QDir(dir).exists()) {
        m_lastError = QStringLiteral("AFX pack directory not found: %1").arg(dir);
        qCWarning(lcNvAfx) << "NvidiaAfxFilter:" << m_lastError;
        return;
    }
    if (!loadRuntime(dir))
        return;
    for (void*& handle : m_handles) {
        if (!createDenoiser(dir, &handle))
            return;
    }
    m_ready = true;
    qCDebug(lcNvAfx) << "NvidiaAfxFilter: ready (frame =" << m_afxFrame << "@ 48 kHz)";
}

NvidiaAfxFilter::~NvidiaAfxFilter()
{
    teardown();
}

void NvidiaAfxFilter::teardown()
{
    for (void*& handle : m_handles) {
        if (handle && m_api && m_api->DestroyEffect) {
            m_api->DestroyEffect(handle);
        }
        handle = nullptr;
    }
    // Close library handles in reverse order. (CUDA libs are typically retained
    // by the driver; closing is best-effort cleanup.)
    for (auto it = m_dlHandles.rbegin(); it != m_dlHandles.rend(); ++it) {
        if (*it) { afxCloseLibrary(*it); }
    }
    m_dlHandles.clear();
    m_ready = false;
}

// Multi-pass dlopen of every shared lib under the pack's runtime dirs, so
// dependency ordering resolves itself (a lib whose NEEDED deps aren't loaded
// yet fails this pass and succeeds on a later one). RTLD_GLOBAL makes each
// lib's symbols/soname visible to satisfy the next.
bool NvidiaAfxFilter::loadRuntime(const QString& packDir)
{
    // Load only the core library; let the OS loader resolve its CUDA/TensorRT
    // deps and the denoiser FEATURE lib from the pack's runtime dir — mirroring
    // how the SDK's own sample links only against the core. (Pre-loading the
    // whole tree ourselves corrupts CUDA init, so we deliberately do not.)
    //
    //  • Linux: core is nvafx/lib/libnv_audiofx.so; its DT_RPATH
    //    ($ORIGIN/../../external/cuda/lib, $ORIGIN/../../nvafx/lib) resolves the
    //    CUDA/TRT runtime and the feature lib (libnv_audiofx_denoiser.so*).
    //  • Windows: core is bin/NVAudioEffects.dll; the CUDA/TRT DLLs and the
    //    feature DLL sit alongside it in bin/, and LOAD_WITH_ALTERED_SEARCH_PATH
    //    makes that directory the head of the dependency search — the RPATH
    //    analogue. We also pin it on the process default search dirs below.
#if defined(_WIN32)
    const QString coreLib =
        QDir(packDir).filePath(QStringLiteral("bin/NVAudioEffects.dll"));
    const QString coreName = QStringLiteral("NVAudioEffects.dll");
    // Register the pack's bin dir on the default DLL search path so transitive
    // loads (a CUDA DLL pulling in another) also resolve from the pack.
    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const QString binDir = QDir::toNativeSeparators(QDir(packDir).filePath(QStringLiteral("bin")));
    ::AddDllDirectory(reinterpret_cast<const wchar_t*>(binDir.utf16()));
#else
    const QString coreLib =
        QDir(packDir).filePath(QStringLiteral("nvafx/lib/libnv_audiofx.so"));
    const QString coreName = QStringLiteral("libnv_audiofx.so");
#endif
    if (!QFile::exists(coreLib)) {
        m_lastError = QStringLiteral("%1 not found at %2").arg(coreName, coreLib);
        return false;
    }

    void* core = afxLoadLibrary(coreLib);
    if (!core) {
        m_lastError = QStringLiteral("load(%1) failed: %2").arg(coreName, afxLoadError());
        return false;
    }
    m_dlHandles.push_back(core);

    auto sym = [&](const char* n) { return afxGetSymbol(core, n); };
    // Optional: route AFX's own diagnostics to stderr for debugging.
    if (!qgetenv("AETHER_NVAFX_DEBUG").isEmpty()) {
        if (auto initLog = reinterpret_cast<fn_InitLogger>(sym("NvAFX_InitializeLogger")))
            initLog(2 /*INFO*/, 1 /*STDERR*/, nullptr, nullptr, nullptr);
    }
    m_api->CreateEffect  = reinterpret_cast<fn_CreateEffect>(sym("NvAFX_CreateEffect"));
    m_api->DestroyEffect = reinterpret_cast<fn_DestroyEffect>(sym("NvAFX_DestroyEffect"));
    m_api->SetU32        = reinterpret_cast<fn_SetU32>(sym("NvAFX_SetU32"));
    m_api->SetString     = reinterpret_cast<fn_SetString>(sym("NvAFX_SetString"));
    m_api->SetFloat      = reinterpret_cast<fn_SetFloat>(sym("NvAFX_SetFloat"));
    m_api->GetU32        = reinterpret_cast<fn_GetU32>(sym("NvAFX_GetU32"));
    m_api->Load          = reinterpret_cast<fn_Load>(sym("NvAFX_Load"));
    m_api->Run           = reinterpret_cast<fn_Run>(sym("NvAFX_Run"));
    m_api->Reset         = reinterpret_cast<fn_Reset>(sym("NvAFX_Reset"));

    if (!m_api->CreateEffect || !m_api->SetU32 || !m_api->SetString ||
        !m_api->SetFloat || !m_api->GetU32 || !m_api->Load || !m_api->Run ||
        !m_api->DestroyEffect) {
        m_lastError = QStringLiteral("NvAFX_* symbols missing from libnv_audiofx.so");
        return false;
    }
    return true;
}

bool NvidiaAfxFilter::createDenoiser(const QString& packDir, void** handle)
{
    const QString model = findModel(packDir);
    if (model.isEmpty()) {
        m_lastError = QStringLiteral("denoiser model (sm_XX/denoiser_48k.trtpkg) not found");
        return false;
    }

    if (m_api->CreateEffect(EFFECT_DENOISER, handle) != NVAFX_OK || !*handle) {
        m_lastError = QStringLiteral("NvAFX_CreateEffect(denoiser) failed");
        return false;
    }
    // 0 = let the SDK auto-select a supported (Turing+) GPU. On a hybrid
    // Intel-iGPU + NVIDIA-dGPU laptop, 1 ("OS default GPU") may pick the
    // unsupported Intel iGPU and fail Load. (#nvidia-afx)
    m_api->SetU32(*handle, P_USE_DEFAULT_GPU, 0);
    if (m_api->SetU32(*handle, P_SAMPLE_RATE, kSampleRate) != NVAFX_OK)
        m_api->SetU32(*handle, P_SAMPLE_RATE_LEGACY, kSampleRate);
    if (m_api->SetString(*handle, P_MODEL_PATH, model.toLocal8Bit().constData()) != NVAFX_OK) {
        m_lastError = QStringLiteral("NvAFX_SetString(model_path) failed: %1").arg(model);
        return false;
    }
    m_api->SetU32(*handle, P_NUM_STREAMS, 1);
    if (m_api->SetU32(*handle, P_FRAME_IN, kRequestedFrame) != NVAFX_OK)
        m_api->SetU32(*handle, P_FRAME, kRequestedFrame);

    if (m_api->Load(*handle) != NVAFX_OK) {
        m_lastError = QStringLiteral("NvAFX_Load() failed (GPU unsupported or model incompatible)");
        return false;
    }

    unsigned frame = 0;
    if (m_api->GetU32(*handle, P_FRAME, &frame) != NVAFX_OK || frame == 0)
        m_api->GetU32(*handle, P_FRAME_OUT, &frame);
    m_afxFrame = frame > 0 ? static_cast<int>(frame) : kRequestedFrame;

    m_api->SetFloat(*handle, P_INTENSITY, m_intensity.load());
    resetEffect(*handle);
    return true;
}

// A new effect is not guaranteed a clean state. While another denoiser effect
// keeps the SDK loaded, an effect created after one is destroyed starts from
// that effect's leftover recurrent state, and its first ~0.5 s differs from a
// fresh start by up to the full signal level. NvAFX_Reset clears it.
void NvidiaAfxFilter::resetEffect(void* handle)
{
    if (!handle || !m_api->Reset) {
        return;
    }
    bool resetStream = true;
    m_api->Reset(handle, &resetStream, 1);
}

void NvidiaAfxFilter::setIntensity(float ratio)
{
    if (ratio < 0.0f) { ratio = 0.0f; }
    if (ratio > 1.0f) { ratio = 1.0f; }
    m_intensity.store(ratio);
    m_paramsDirty.store(true);
}

// ─── Audio-thread processing (mirrors DeepFilterFilter) ──────────────────────
QByteArray NvidiaAfxFilter::process(const QByteArray& pcmStereo)
{
    if (!m_ready || m_afxFrame <= 0 || pcmStereo.isEmpty())
        return pcmStereo;

    if (m_paramsDirty.exchange(false)) {
        for (void* handle : m_handles)
            m_api->SetFloat(handle, P_INTENSITY, m_intensity.load());
    }

    const auto* src = reinterpret_cast<const float*>(pcmStereo.constData());
    const int stereoFrames = pcmStereo.size() / (2 * static_cast<int>(sizeof(float)));

    // Both channels see the same sample counts through identically configured
    // resamplers, so they reach the same whole-frame count and the two
    // effects advance in lockstep.
    int frames = std::numeric_limits<int>::max();
    std::array<int, 2> total{0, 0};
    for (int channel = 0; channel < 2; ++channel) {
        // 1. Split out this channel and convert legacy24 to 48; native48
        // needs no SRC.
        auto& channelInput = m_channelInput[channel];
        channelInput.resize(stereoFrames);
        for (int i = 0; i < stereoFrames; ++i) {
            channelInput[i] = src[i * 2 + channel];
        }
        const QByteArray input48k = m_up[channel]
            ? m_up[channel]->process(channelInput.data(), stereoFrames)
            : QByteArray(reinterpret_cast<const char*>(channelInput.data()),
                         stereoFrames * static_cast<int>(sizeof(float)));

        // 2. Accumulate to whole AFX frames.
        const int prev = m_inAccum[channel].size() / static_cast<int>(sizeof(float));
        m_inAccum[channel].append(input48k);
        total[channel] = prev + input48k.size() / static_cast<int>(sizeof(float));
        frames = std::min(frames, total[channel] / m_afxFrame);
    }

    if (frames > 0) {
        const int consumed = frames * m_afxFrame;
        int outputFrames = std::numeric_limits<int>::max();
        for (int channel = 0; channel < 2; ++channel) {
            auto* accum = reinterpret_cast<float*>(m_inAccum[channel].data());
            auto& scratch = m_runScratch[channel];
            if (scratch.size() < static_cast<size_t>(consumed)) {
                scratch.resize(consumed);   // grows to steady state, then alloc-free
            }
            float* out = scratch.data();
            for (int f = 0; f < frames; ++f) {
                const float* in[1]  = { &accum[f * m_afxFrame] };
                float*       op[1]  = { &out[f * m_afxFrame] };
                if (m_api->Run(m_handles[channel], in, op,
                               static_cast<unsigned>(m_afxFrame), 1) != NVAFX_OK) {
                    // On a transient failure, pass the frame through unprocessed.
                    std::memcpy(op[0], in[0], m_afxFrame * sizeof(float));
                }
            }
            m_inAccum[channel].remove(0, consumed * static_cast<int>(sizeof(float)));

            // 3. Convert only legacy24.
            m_channelOutput[channel] = m_down[channel]
                ? m_down[channel]->process(out, consumed)
                : QByteArray(reinterpret_cast<const char*>(out),
                             consumed * static_cast<int>(sizeof(float)));
            outputFrames = std::min(
                outputFrames,
                static_cast<int>(m_channelOutput[channel].size() / sizeof(float)));
        }
        // Identical resamplers fed identical counts stay in lockstep, so the
        // min above never drops a sample. A mismatch would be a silent,
        // cumulative L/R skew; make it loud in debug builds.
        Q_ASSERT(m_channelOutput[0].size() == m_channelOutput[1].size());

        const auto* left = reinterpret_cast<const float*>(m_channelOutput[0].constData());
        const auto* right = reinterpret_cast<const float*>(m_channelOutput[1].constData());
        const int start = m_outAccum.size() / static_cast<int>(sizeof(float));
        m_outAccum.resize((start + outputFrames * 2) * static_cast<int>(sizeof(float)));
        auto* stereo = reinterpret_cast<float*>(m_outAccum.data()) + start;
        for (int i = 0; i < outputFrames; ++i) {
            stereo[i * 2] = std::clamp(left[i], -1.0f, 1.0f);
            stereo[i * 2 + 1] = std::clamp(right[i], -1.0f, 1.0f);
        }
    }

    // 4. Return exactly the input byte count. Use a read cursor instead of an
    //    O(n) front-erase every block; compact only once the consumed prefix
    //    grows past the unread tail.
    const int needed = pcmStereo.size();
    if (m_outAccum.size() - m_outReadPos >= needed) {
        QByteArray result(m_outAccum.constData() + m_outReadPos, needed);
        m_outReadPos += needed;
        if (m_outReadPos >= m_outAccum.size()) {
            m_outAccum.clear();
            m_outReadPos = 0;
        } else if (m_outReadPos > m_outAccum.size() - m_outReadPos) {
            m_outAccum.remove(0, m_outReadPos);
            m_outReadPos = 0;
        }
        return result;
    }
    return QByteArray(needed, '\0');  // priming silence at startup
}

void NvidiaAfxFilter::createResamplers()
{
    // Resampler has no reset, so a flush rebuilds them.
    for (int channel = 0; channel < 2; ++channel) {
        if (m_sampleRate == 24000) {
            m_up[channel] = std::make_unique<Resampler>(24000, 48000);
            m_down[channel] = std::make_unique<Resampler>(48000, 24000);
        }
    }
}

void NvidiaAfxFilter::reset()
{
    // Flush jitter accumulators, rebuild the resamplers and clear the effects'
    // recurrent state so no stale or pre-discontinuity audio carries into the
    // next block.
    for (void* handle : m_handles) {
        resetEffect(handle);
    }
    for (auto& accum : m_inAccum) {
        accum.clear();
    }
    m_outAccum.clear();
    m_outReadPos = 0;
    createResamplers();
}

} // namespace AetherSDR

#endif // HAVE_NVIDIA_AFX
