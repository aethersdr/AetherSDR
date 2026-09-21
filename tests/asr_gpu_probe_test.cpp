// Regression test for #4535: the ASR GPU probe must return, promptly.
//
// On the original bug, the first ggml touch compiled the embedded Metal shader
// SOURCE on the calling thread via Apple's runtime compiler — which can
// live-lock on Intel-GPU Macs (measured: no completion in 75 minutes). The fix
// embeds a precompiled .metallib, loaded with newLibraryWithData, so that
// compiler is never invoked. What this test asserts depends on which build it
// is compiled into, because the two builds have different hazards:
//
//   Precompiled build (AETHER_ASR_METAL_PRECOMPILED — the default, and what
//   every release ships): there is no host gate, so every Mac enumerates. The
//   assertion is that enumeration — registry init, Metal device init, embedded
//   metallib load — returns, and returns promptly. That is the call that never
//   came back on the pre-fix build.
//
//   Source-embed fallback build: Apple's runtime compiler is reachable again,
//   so asrMetalUsableHost() keeps Intel Macs from enumerating Metal at all. The
//   assertion is that the gate answers empty, and answers before any ggml work
//   (sub-second). Case order matters here: the gated case must run before the
//   AETHER_ASR_FORCE_METAL one, because the forced probe initializes the
//   process-wide ggml registry — exactly what the gate exists to prevent.
//
// A detached watchdog turns a regression into a fast, labeled failure instead
// of a ctest timeout.
//
// With AETHER_ASR_EXPECT_PRECOMPILED=1 (set by CI) the test additionally asserts
// that a Metal device really was enumerated and that ggml logged the *compiled*
// embed branch — without that, a host with no GPU, or a build that quietly fell
// back to the source embed when the offline Metal toolchain was missing, would
// pass this test while exercising none of what it guards.

#include "asr/WhisperAsrBackend.h"

#include <ggml.h>

#include <QElapsedTimer>
#include <QtGlobal>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifdef Q_OS_MACOS
#include <sys/sysctl.h>

static bool hostIsAppleSilicon()
{
    int isArm64 = 0;
    size_t size = sizeof(isArm64);
    if (sysctlbyname("hw.optional.arm64", &isArm64, &size, nullptr, 0) != 0) {
        return false;
    }
    return isArm64 == 1;
}
#endif

using namespace AetherSDR;

// ggml narrates which library path it took ("using embedded precompiled metal
// library" vs "using embedded metal library"). Capturing it is what makes the
// silent-degradation mode visible: the CMake toolchain probe falls back to the
// source embed with only a message(WARNING), which scrolls past in a 2000-line
// build, so a green build is not by itself evidence the compiled embed engaged.
static std::string g_ggmlLog;

static void captureGgmlLog(enum ggml_log_level, const char* text, void*)
{
    if (text) {
        g_ggmlLog += text;
    }
}

int main()
{
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(120));
        std::fprintf(stderr, "[FAIL] GPU probe did not return within 120 s "
                             "(#4535 regression: runtime shader compile on the "
                             "probe path)\n");
        std::_Exit(2);
    }).detach();

    ggml_log_set(captureGgmlLog, nullptr);

    // ---- #4502 pure contracts -------------------------------------------------
    // Host-independent, and run before anything touches ggml so a GPU-less or
    // hostile-driver machine still exercises them.
    //
    // Default resolution: the first usable device wins; no usable device means
    // CPU (-1). Selecting a device that cannot run the decode is the failure
    // these pin — it yields wrong output at every tier, and on a driver that
    // cannot create a device at all it is the crash path of #4502.
    {
        const std::vector<AsrGpuDevice> noneUsable = {
            {0, QStringLiteral("dGPU"), false}, {1, QStringLiteral("iGPU"), false}};
        const std::vector<AsrGpuDevice> secondUsable = {
            {0, QStringLiteral("dGPU"), false}, {1, QStringLiteral("iGPU"), true}};
        const std::vector<AsrGpuDevice> bothUsable = {
            {0, QStringLiteral("dGPU"), true}, {1, QStringLiteral("iGPU"), true}};
        if (asrResolveDefaultGpuIndex({}) != -1
            || asrResolveDefaultGpuIndex(noneUsable) != -1
            || asrResolveDefaultGpuIndex(secondUsable) != 1
            || asrResolveDefaultGpuIndex(bothUsable) != 0) {
            std::fprintf(stderr, "[FAIL] asrResolveDefaultGpuIndex contract broken "
                                 "(empty/none-usable/second-usable/first-usable) "
                                 "(#4502)\n");
            return 1;
        }
        std::printf("[ok] #4502 default-resolution contract\n");
    }

    // Session failure latch: one-way, and it must survive being asked twice.
    // A device latched here is never handed to whisper again this process —
    // that is what stops the second device-creation attempt, which is the
    // attempt that faults below any catchable level (#4502).
    {
        // A high index no real enumeration reaches, so latching it cannot
        // change what the hardware cases below are allowed to resolve to.
        constexpr int kSyntheticDevice = 4502;
        if (asrGpuDeviceFailed(kSyntheticDevice)) {
            std::fprintf(stderr, "[FAIL] latch reports a device failed before "
                                 "anything marked it (#4502)\n");
            return 1;
        }
        asrMarkGpuDeviceFailed(kSyntheticDevice);
        if (!asrGpuDeviceFailed(kSyntheticDevice)) {
            std::fprintf(stderr, "[FAIL] latch did not hold after marking (#4502)\n");
            return 1;
        }
        asrMarkGpuDeviceFailed(kSyntheticDevice); // idempotent
        if (!asrGpuDeviceFailed(kSyntheticDevice)) {
            std::fprintf(stderr, "[FAIL] latch cleared on a second mark (#4502)\n");
            return 1;
        }
        // CPU (-1) is the fallback target and must never be latched out.
        asrMarkGpuDeviceFailed(-1);
        if (asrGpuDeviceFailed(-1)) {
            std::fprintf(stderr, "[FAIL] CPU was latched out - the fallback target "
                                 "must always stay available (#4502)\n");
            return 1;
        }
        std::printf("[ok] #4502 session failure latch (one-way, idempotent, CPU exempt)\n");
    }

    // GPU-default tier reconciliation: raising to the GPU default must require
    // a usable GPU, and an AUTO-raised GPU default must walk back to the base
    // default when resolution falls off the GPU — running the heaviest tier
    // on CPU is the "backlog climbing, no text" symptom the load-time
    // fallback arm produced (#4767 review; the drill that verified the PR ran
    // the probe-throw arm, where the tier was never raised, so only a pinned
    // contract catches this). An explicit operator choice is never touched.
    {
        const QString turbo = QStringLiteral("large-v3-turbo");
        const QString base = QStringLiteral("base.en");
        const QString small = QStringLiteral("small.en");

        // Startup on a usable GPU: raise, and remember it was automatic.
        AsrTierResolution r = asrReconcileDefaultTier(base, true, false, true, turbo, base);
        if (r.tierId != turbo || !r.gpuDefaultActive) {
            std::fprintf(stderr, "[FAIL] usable GPU + default wanted did not raise "
                                 "to the GPU tier (#4767)\n");
            return 1;
        }
        // Load-time fallback (the want-flag is already consumed by then): an
        // auto-raised Turbo must walk back once resolution is CPU. This is
        // the regression the review found — the arm where the raise had
        // already happened and only the walk-back can undo it.
        r = asrReconcileDefaultTier(turbo, false, true, false, turbo, base);
        if (r.tierId != base || r.gpuDefaultActive) {
            std::fprintf(stderr, "[FAIL] auto-raised GPU tier survived a CPU "
                                 "resolution - Turbo keeps running on CPU (#4767)\n");
            return 1;
        }
        // Explicitly chosen Turbo (never auto-raised) on CPU: untouched.
        r = asrReconcileDefaultTier(turbo, false, false, false, turbo, base);
        if (r.tierId != turbo || r.gpuDefaultActive) {
            std::fprintf(stderr, "[FAIL] an explicit Turbo choice was walked back "
                                 "(#4767)\n");
            return 1;
        }
        // Explicit non-default tier while a usable GPU resolves: untouched.
        r = asrReconcileDefaultTier(small, false, false, true, turbo, base);
        if (r.tierId != small) {
            std::fprintf(stderr, "[FAIL] an explicit tier was overridden by the "
                                 "GPU default (#4767)\n");
            return 1;
        }
        // Auto-raised Turbo re-resolved onto ANOTHER usable GPU: stays raised,
        // stays automatic (a later CPU fallback must still walk it back).
        r = asrReconcileDefaultTier(turbo, false, true, true, turbo, base);
        if (r.tierId != turbo || !r.gpuDefaultActive) {
            std::fprintf(stderr, "[FAIL] auto-raised GPU tier did not survive a "
                                 "move to another usable GPU (#4767)\n");
            return 1;
        }
        std::printf("[ok] #4767 GPU-default tier reconciliation "
                    "(raise gated on usable GPU, auto-raise walks back, "
                    "explicit choices untouched)\n");
    }

    // ---- #5190 whisper/ggml log routing policy --------------------------------
    // Pure: what reaches the log file out of the callback stream. Level numbers
    // are ggml_log_level's (the .cpp static_asserts the mirror).
    {
        using Asm = AsrLibLogAssembler;
        constexpr int kInfo = 2; // GGML_LOG_LEVEL_INFO

        // MEASURED (09-16 ballast bench, Linux RTX 5060, main 85fdf816, the
        // stderr capture of the run that then segfaulted): emitted by
        // GGML_LOG_ERROR at ggml-alloc.c:1133.
        const char* allocFailed =
            "alloc_tensor_range: failed to allocate Vulkan1 buffer of size 551900160\n";
        // MEASURED (same capture): one of the INFO lines a load prints.
        const char* infoLine = "whisper_init_with_params_no_state: use gpu    = 1\n";

        Asm a;
        std::vector<Asm::Line> got = a.feed(Asm::kLevelError, allocFailed);
        if (got.size() != 1 || !got[0].error
            || got[0].text
                != QStringLiteral("alloc_tensor_range: failed to allocate Vulkan1 "
                                  "buffer of size 551900160")) {
            std::fprintf(stderr, "[FAIL] a ggml ERROR line was not forwarded whole, "
                                 "as an error, without its newline (#5190)\n");
            return 1;
        }
        got = a.feed(kInfo, infoLine);
        if (!got.empty()) {
            std::fprintf(stderr, "[FAIL] an INFO line was forwarded - a model load "
                                 "prints dozens of them (#5190)\n");
            return 1;
        }
        // CONSTRUCTED from here on: transformation logic only (fragment joining),
        // no claim that whisper emits these shapes.
        got = a.feed(Asm::kLevelCont, " continuation of an INFO message\n");
        if (!got.empty()) {
            std::fprintf(stderr, "[FAIL] a CONT fragment of a dropped message was "
                                 "forwarded (#5190)\n");
            return 1;
        }
        got = a.feed(Asm::kLevelWarn, "first half,");
        if (!got.empty()) {
            std::fprintf(stderr, "[FAIL] an unterminated fragment was forwarded "
                                 "before its line completed (#5190)\n");
            return 1;
        }
        got = a.feed(Asm::kLevelCont, " second half\nnext line\n");
        if (got.size() != 2 || got[0].error
            || got[0].text != QStringLiteral("first half, second half")
            || got[1].text != QStringLiteral("next line")) {
            std::fprintf(stderr, "[FAIL] CONT fragments were not joined onto the "
                                 "WARN message and split into whole lines (#5190)\n");
            return 1;
        }
        // Two same-level fragments are two messages, not one: the vendored
        // whisper/ggml repeat the level for multi-part output instead of using
        // CONT, and only CONT joins.
        got = a.feed(Asm::kLevelWarn, "part one\n");
        std::vector<Asm::Line> second = a.feed(Asm::kLevelWarn, "part two\n");
        if (got.size() != 1 || second.size() != 1
            || got[0].text != QStringLiteral("part one")
            || second[0].text != QStringLiteral("part two")) {
            std::fprintf(stderr, "[FAIL] two same-level messages were not kept as "
                                 "two lines (#5190)\n");
            return 1;
        }
        // An unterminated message is finished by the next message, not lost and
        // not glued onto it.
        a.feed(Asm::kLevelError, "no newline");
        got = a.feed(kInfo, "unrelated info\n");
        if (got.size() != 1 || !got[0].error || got[0].text != QStringLiteral("no newline")) {
            std::fprintf(stderr, "[FAIL] an unterminated ERROR was lost or merged "
                                 "into the next message (#5190)\n");
            return 1;
        }
        std::printf("[ok] #5190 whisper/ggml log routing "
                    "(WARN+ERROR forwarded whole, INFO dropped, CONT joined)\n");
    }

    // VRAM gate on the automatic raise to the GPU-default tier (#4972): "a GPU
    // exists" must not be enough to select a 1.6 GB model.
    {
        constexpr quint64 kMiB = 1024ull * 1024ull;
        // Copy of the "large-v3-turbo" sizeBytes in AsrModelCatalog.cpp (this
        // target does not link the catalog) — keep the two in step.
        constexpr qint64 kTurboBytes = 1624555275;

        // MEASURED (#4972 bench, RTX 5060 Laptop 8151 MiB under VRAM ballast,
        // 2026-09-16): the app logged "VRAM free 1126 of 8151 MB" and enabling
        // the auto-raised tier took SIGSEGV in the whisper model load.
        if (asrTierFitsVram(1126 * kMiB, 8151 * kMiB, kTurboBytes)) {
            std::fprintf(stderr, "[FAIL] 1126 MB free was judged enough for the "
                                 "1.6 GB tier (#4972)\n");
            return 1;
        }
        // MEASURED (#5730 reporter log, GTX 1050, 2026-09-15): "VRAM free 1809
        // of 2176 MB". The tier occupies 1818 MiB once loaded (same bench), so
        // a 2 GB-class card is never auto-raised; choosing it stays possible.
        if (asrTierFitsVram(1809 * kMiB, 2176 * kMiB, kTurboBytes)) {
            std::fprintf(stderr, "[FAIL] a 2 GB-class card (1809 MB free) was "
                                 "judged to fit the 1.6 GB tier (#4972)\n");
            return 1;
        }
        // MEASURED (same bench, no ballast): "VRAM free 7360 of 8151 MB".
        if (!asrTierFitsVram(7360 * kMiB, 8151 * kMiB, kTurboBytes)) {
            std::fprintf(stderr, "[FAIL] an 8 GB card with 7360 MB free was "
                                 "refused the GPU-default tier (#4972)\n");
            return 1;
        }
        // CONSTRUCTED: the boundary itself — weights + headroom fits, one byte
        // less does not.
        const quint64 need = static_cast<quint64>(kTurboBytes) + kAsrTierVramHeadroomBytes;
        if (!asrTierFitsVram(need, 4096 * kMiB, kTurboBytes)
            || asrTierFitsVram(need - 1, 4096 * kMiB, kTurboBytes)) {
            std::fprintf(stderr, "[FAIL] VRAM gate boundary is not weights + "
                                 "headroom (#4972)\n");
            return 1;
        }
        // CONSTRUCTED input on a MEASURED total (2176 MB, #5730 reporter log):
        // ggml-vulkan reports free == total for a device without
        // VK_EXT_memory_budget (ggml_backend_vk_get_device_memory), so the same
        // card can present as 2176 of 2176 MB free. No capture of a driver in
        // that mode exists; the row pins that the answer does not depend on it.
        if (asrTierFitsVram(2176 * kMiB, 2176 * kMiB, kTurboBytes)) {
            std::fprintf(stderr, "[FAIL] a 2 GB-class card reporting free == total "
                                 "was judged to fit the 1.6 GB tier (#4972)\n");
            return 1;
        }
        // CONSTRUCTED: the total boundary — need + desktop reserve fits, one
        // byte less does not, with free memory ample in both.
        const quint64 needTotal = need + kAsrTierVramDesktopReserveBytes;
        if (!asrTierFitsVram(needTotal, needTotal, kTurboBytes)
            || asrTierFitsVram(needTotal - 1, needTotal - 1, kTurboBytes)) {
            std::fprintf(stderr, "[FAIL] VRAM gate total boundary is not weights + "
                                 "headroom + desktop reserve (#4972)\n");
            return 1;
        }
        // CONSTRUCTED: memory unknown (AsrGpuDevice leaves both 0 when the
        // device could not be asked) is not "too small" — previous behaviour.
        if (!asrTierFitsVram(0, 0, kTurboBytes)) {
            std::fprintf(stderr, "[FAIL] unknown VRAM was treated as too small "
                                 "(#4972)\n");
            return 1;
        }
        // CONSTRUCTED: an unknown tier size cannot refuse a device.
        if (!asrTierFitsVram(64 * kMiB, 2048 * kMiB, 0)) {
            std::fprintf(stderr, "[FAIL] an unknown tier size refused a device "
                                 "(#4972)\n");
            return 1;
        }
        std::printf("[ok] #4972 VRAM gate on the GPU-default tier "
                    "(weights + headroom, total clears a desktop reserve, "
                    "unknown memory passes)\n");
    }

    QElapsedTimer timer;

#ifdef Q_OS_MACOS
    qunsetenv("AETHER_ASR_FORCE_METAL");

    timer.start();
    const std::vector<AsrGpuDevice> gated = asrGpuDevices();
    const qint64 gatedMs = timer.elapsed();

#ifdef AETHER_ASR_METAL_PRECOMPILED
    // No host gate on this build: the runtime shader compiler is unreachable, so
    // an Intel Mac enumerates like any other and ggml's own per-op capability
    // checks decide what runs. The #4535 assertion is that it comes back fast.
    // 30 s is far above any healthy first touch (68 ms on CI's Apple Silicon,
    // 730 ms on the reporter's Intel MBP) and far below the live-lock, which
    // never returns at all.
    if (gatedMs > 30000) {
        std::fprintf(stderr, "[FAIL] enumeration took %lld ms on a precompiled "
                             "build - nothing on this path should be compiling "
                             "shaders (#4535)\n",
                     static_cast<long long>(gatedMs));
        return 1;
    }
    std::printf("[ok] precompiled build, no host gate: %zu device(s) in %lld ms%s\n",
                gated.size(), static_cast<long long>(gatedMs),
                hostIsAppleSilicon() ? " (Apple Silicon)" : " (Intel)");
#else
    if (!hostIsAppleSilicon()) {
        if (!gated.empty()) {
            std::fprintf(stderr, "[FAIL] Intel Mac was offered %zu Metal device(s) "
                                 "on a source-embed build without "
                                 "AETHER_ASR_FORCE_METAL\n",
                         gated.size());
            return 1;
        }
        if (gatedMs > 1000) {
            std::fprintf(stderr, "[FAIL] gated probe took %lld ms - the gate "
                                 "must answer before any ggml work\n",
                         static_cast<long long>(gatedMs));
            return 1;
        }
        std::printf("[ok] Intel Mac gate (source-embed build): no Metal offered, "
                    "answered in %lld ms\n", static_cast<long long>(gatedMs));
    } else {
        std::printf("[ok] Apple Silicon: %zu device(s) in %lld ms\n",
                    gated.size(), static_cast<long long>(gatedMs));
    }
#endif

    qputenv("AETHER_ASR_FORCE_METAL", "1");
#endif

    timer.start();
    const std::vector<AsrGpuDevice> devices = asrGpuDevices();
    const qint64 probeMs = timer.elapsed();

    std::printf("[ok] full GPU probe returned: %zu device(s) in %lld ms\n",
                devices.size(), static_cast<long long>(probeMs));
    for (const AsrGpuDevice& d : devices) {
        std::printf("     device %d: %s (usable=%s)\n", d.index, qPrintable(d.name),
                    d.usable ? "true" : "false");
    }

    // #4502 default-selection contract, on the real enumeration this time: the
    // resolved default is either CPU (-1) or a device that actually passed the
    // probe. Never a device we already know cannot run the decode.
    {
        const int def = asrResolveDefaultGpuIndex(devices);
        if (def != -1) {
            const auto it = std::find_if(devices.begin(), devices.end(),
                                         [def](const AsrGpuDevice& d) { return d.index == def; });
            if (it == devices.end() || !it->usable) {
                std::fprintf(stderr, "[FAIL] default resolved to device %d, which is not "
                                     "a usable enumerated device (#4502)\n", def);
                return 1;
            }
        }
        std::printf("[ok] #4502 default resolution on this host: %d\n", def);
    }

    // A device the probe rejected must also be latched, so a later explicit
    // pick or a restored preference cannot walk back into it. (A probe that
    // merely reports unusable without throwing does not latch — that device
    // was created fine — so this only asserts the implication one way.)
    for (const AsrGpuDevice& d : devices) {
        if (asrGpuDeviceFailed(d.index) && d.usable) {
            std::fprintf(stderr, "[FAIL] device %d is latched as failed but still "
                                 "reported usable (#4502)\n", d.index);
            return 1;
        }
    }
    std::printf("[ok] #4502 latched devices are never reported usable\n");

#ifdef Q_OS_MACOS
    // Everything below is about the embedded-metallib load path, which only
    // exists on macOS — on Linux/Windows the probe above (Vulkan or CPU) is the
    // whole test.
    //
    // A probe that enumerates no GPU never reaches newLibraryWithData, so on a
    // host with no Metal device this test says nothing about the load path it
    // exists to guard. CI sets AETHER_ASR_EXPECT_PRECOMPILED=1 to turn that
    // silent vacuousness into a failure; a developer running ctest on a GPU-less
    // box (or with -DENABLE_ASR_METAL_PRECOMPILE=OFF) just gets the note.
    const bool expectPrecompiled = qEnvironmentVariableIsSet("AETHER_ASR_EXPECT_PRECOMPILED");

    if (devices.empty()) {
        if (expectPrecompiled) {
            std::fprintf(stderr, "[FAIL] AETHER_ASR_EXPECT_PRECOMPILED is set but no "
                                 "Metal device was enumerated - the embedded-metallib "
                                 "load path never ran, so this test proved nothing\n");
            return 1;
        }
        std::printf("[note] no GPU enumerated - embedded-metallib load path not "
                    "exercised on this host\n");
        return 0;
    }

    // "using embedded precompiled metal library" is the compiled-embed branch;
    // "using embedded metal library" is the source embed that runs the runtime
    // shader compiler — the exact #4535 path.
    const bool loadedPrecompiled =
        g_ggmlLog.find("using embedded precompiled metal library") != std::string::npos;

    if (loadedPrecompiled) {
        std::printf("[ok] embedded PRECOMPILED metallib loaded (no runtime shader compile)\n");
    } else if (expectPrecompiled) {
        std::fprintf(stderr, "[FAIL] a Metal device initialized without loading the "
                             "embedded precompiled metallib - the build fell back to "
                             "the embedded-source runtime-compile path (#4535). Check "
                             "the configure log for the offline Metal toolchain "
                             "warning.\n");
        return 1;
    } else {
        std::printf("[note] precompiled metallib not reported - build may have "
                    "fallen back to source embed\n");
    }
#endif // Q_OS_MACOS

    return 0;
}
