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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

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
        std::printf("     device %d: %s\n", d.index, qPrintable(d.name));
    }

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
