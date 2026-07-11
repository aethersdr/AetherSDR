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
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif
#include <cstring>
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
NvidiaAfxFilter::NvidiaAfxFilter(const QString& packDir)
    : m_api(std::make_unique<Api>())
    , m_up(std::make_unique<Resampler>(24000, 48000))
    , m_down(std::make_unique<Resampler>(48000, 24000))
{
    const QString dir = resolvePackDir(packDir);
    if (!QDir(dir).exists()) {
        m_lastError = QStringLiteral("AFX pack directory not found: %1").arg(dir);
        qCWarning(lcNvAfx) << "NvidiaAfxFilter:" << m_lastError;
        return;
    }
    if (!loadRuntime(dir))
        return;
    if (!createDenoiser(dir))
        return;
    m_ready = true;
    qCDebug(lcNvAfx) << "NvidiaAfxFilter: ready (frame =" << m_afxFrame << "@ 48 kHz)";
}

NvidiaAfxFilter::~NvidiaAfxFilter()
{
    teardown();
}

void NvidiaAfxFilter::teardown()
{
    if (m_handle && m_api && m_api->DestroyEffect) {
        m_api->DestroyEffect(m_handle);
        m_handle = nullptr;
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

    if (!m_api->CreateEffect || !m_api->SetU32 || !m_api->SetString ||
        !m_api->SetFloat || !m_api->GetU32 || !m_api->Load || !m_api->Run ||
        !m_api->DestroyEffect) {
        m_lastError = QStringLiteral("NvAFX_* symbols missing from libnv_audiofx.so");
        return false;
    }
    return true;
}

bool NvidiaAfxFilter::createDenoiser(const QString& packDir)
{
    const QString model = findModel(packDir);
    if (model.isEmpty()) {
        m_lastError = QStringLiteral("denoiser model (sm_XX/denoiser_48k.trtpkg) not found");
        return false;
    }

    if (m_api->CreateEffect(EFFECT_DENOISER, &m_handle) != NVAFX_OK || !m_handle) {
        m_lastError = QStringLiteral("NvAFX_CreateEffect(denoiser) failed");
        return false;
    }
    // 0 = let the SDK auto-select a supported (Turing+) GPU. On a hybrid
    // Intel-iGPU + NVIDIA-dGPU laptop, 1 ("OS default GPU") may pick the
    // unsupported Intel iGPU and fail Load. (#nvidia-afx)
    m_api->SetU32(m_handle, P_USE_DEFAULT_GPU, 0);
    if (m_api->SetU32(m_handle, P_SAMPLE_RATE, kSampleRate) != NVAFX_OK)
        m_api->SetU32(m_handle, P_SAMPLE_RATE_LEGACY, kSampleRate);
    if (m_api->SetString(m_handle, P_MODEL_PATH, model.toLocal8Bit().constData()) != NVAFX_OK) {
        m_lastError = QStringLiteral("NvAFX_SetString(model_path) failed: %1").arg(model);
        return false;
    }
    m_api->SetU32(m_handle, P_NUM_STREAMS, 1);
    if (m_api->SetU32(m_handle, P_FRAME_IN, kRequestedFrame) != NVAFX_OK)
        m_api->SetU32(m_handle, P_FRAME, kRequestedFrame);

    if (m_api->Load(m_handle) != NVAFX_OK) {
        m_lastError = QStringLiteral("NvAFX_Load() failed (GPU unsupported or model incompatible)");
        return false;
    }

    unsigned frame = 0;
    if (m_api->GetU32(m_handle, P_FRAME, &frame) != NVAFX_OK || frame == 0)
        m_api->GetU32(m_handle, P_FRAME_OUT, &frame);
    m_afxFrame = frame > 0 ? static_cast<int>(frame) : kRequestedFrame;

    m_api->SetFloat(m_handle, P_INTENSITY, m_intensity.load());
    return true;
}

void NvidiaAfxFilter::setIntensity(float ratio)
{
    if (ratio < 0.0f) { ratio = 0.0f; }
    if (ratio > 1.0f) { ratio = 1.0f; }
    m_intensity.store(ratio);
    m_paramsDirty.store(true);
}

// ─── Audio-thread processing (mirrors DeepFilterFilter) ──────────────────────
QByteArray NvidiaAfxFilter::process(const QByteArray& pcm24kStereo)
{
    if (!m_ready || m_afxFrame <= 0 || pcm24kStereo.isEmpty())
        return pcm24kStereo;

    if (m_paramsDirty.exchange(false))
        m_api->SetFloat(m_handle, P_INTENSITY, m_intensity.load());

    const auto* src = reinterpret_cast<const float*>(pcm24kStereo.constData());
    const int stereoFrames = pcm24kStereo.size() / (2 * static_cast<int>(sizeof(float)));
    m_stereoAdapter.pushDryStereo(pcm24kStereo);

    // 1. 24 kHz stereo float32 → 48 kHz mono float32. Keep the dry stereo
    // queued so the BNR attenuation can be applied without collapsing pan.
    m_mono24k.resize(stereoFrames);
    for (int i = 0; i < stereoFrames; ++i) {
        m_mono24k[i] = 0.5f * (src[i * 2] + src[i * 2 + 1]);
    }
    QByteArray mono48k = m_up->process(m_mono24k.data(), stereoFrames);
    const auto* mono = reinterpret_cast<const float*>(mono48k.constData());
    const int monoSamples = mono48k.size() / static_cast<int>(sizeof(float));

    // 2. Accumulate and process complete AFX frames
    const int prev = m_inAccum.size() / static_cast<int>(sizeof(float));
    m_inAccum.resize((prev + monoSamples) * sizeof(float));
    std::memcpy(m_inAccum.data() + prev * sizeof(float), mono, monoSamples * sizeof(float));

    const int total = prev + monoSamples;
    const int frames = total / m_afxFrame;
    if (frames > 0) {
        auto* accum = reinterpret_cast<float*>(m_inAccum.data());
        const size_t outN = static_cast<size_t>(frames) * m_afxFrame;
        if (m_runScratch.size() < outN) {
            m_runScratch.resize(outN);   // grows to steady state, then alloc-free
        }
        float* out = m_runScratch.data();
        for (int f = 0; f < frames; ++f) {
            const float* in[1]  = { &accum[f * m_afxFrame] };
            float*       op[1]  = { &out[f * m_afxFrame] };
            if (m_api->Run(m_handle, in, op,
                           static_cast<unsigned>(m_afxFrame), 1) != NVAFX_OK) {
                // On a transient failure, pass the frame through unprocessed.
                std::memcpy(op[0], in[0], m_afxFrame * sizeof(float));
            }
        }
        const int consumed = frames * m_afxFrame;
        const int leftover = total - consumed;
        if (leftover > 0) {
            std::memmove(m_inAccum.data(),
                         reinterpret_cast<const char*>(&accum[consumed]),
                         leftover * sizeof(float));
            m_inAccum.resize(leftover * sizeof(float));
        } else {
            m_inAccum.clear();
        }
        // 3. 48 kHz mono float32 → 24 kHz mono float32, then re-apply the
        // shared BNR envelope to the delayed dry stereo.
        const QByteArray downsampled = m_down->process(out, consumed);
        const auto* downsampledMono = reinterpret_cast<const float*>(downsampled.constData());
        const int downsampledFrames = downsampled.size() / static_cast<int>(sizeof(float));
        m_outAccum.append(m_stereoAdapter.takeProcessedMono(downsampledMono, downsampledFrames));
    }

    // 4. Return exactly the input byte count. Use a read cursor instead of an
    //    O(n) front-erase every block; compact only once the consumed prefix
    //    grows past the unread tail.
    const int needed = pcm24kStereo.size();
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

void NvidiaAfxFilter::reset()
{
    // Flush jitter accumulators and rebuild the resamplers (Resampler has no
    // reset) so no stale or pre-discontinuity audio carries into the next block.
    m_inAccum.clear();
    m_outAccum.clear();
    m_outReadPos = 0;
    m_stereoAdapter.reset();
    m_up   = std::make_unique<Resampler>(24000, 48000);
    m_down = std::make_unique<Resampler>(48000, 24000);
}

} // namespace AetherSDR

#endif // HAVE_NVIDIA_AFX
