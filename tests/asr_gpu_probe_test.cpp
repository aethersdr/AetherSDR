// Regression test for #4535: the ASR GPU probe must return, promptly.
//
// On the original bug, the first ggml touch compiled the embedded Metal shader
// SOURCE on the calling thread via Apple's runtime compiler — which can
// live-lock on Intel-GPU Macs (measured: no completion in 75 minutes). The fix
// embeds a precompiled .metallib (loaded with newLibraryWithData, no runtime
// compiler) and gates Metal to Apple Silicon at the probe level, so this test
// asserts both halves:
//   1. macOS, non-Apple-Silicon, no override: asrGpuDevices() is empty and
//      answers without touching ggml at all (sub-second).
//   2. AETHER_ASR_FORCE_METAL=1 (or any non-mac / Apple Silicon host): the
//      real ggml enumeration runs — registry init, Metal device init, embedded
//      metallib load — and returns. On the pre-fix build this is the call that
//      never came back.
// Case order matters: the gated case must run before the forced one, because
// the forced probe initializes the process-wide ggml registry — exactly what
// the gate exists to prevent.
//
// A detached watchdog turns a regression into a fast, labeled failure instead
// of a ctest timeout.

#include "asr/WhisperAsrBackend.h"

#include <QElapsedTimer>
#include <QtGlobal>

#include <chrono>
#include <cstdio>
#include <cstdlib>
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

int main()
{
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(120));
        std::fprintf(stderr, "[FAIL] GPU probe did not return within 120 s "
                             "(#4535 regression: runtime shader compile on the "
                             "probe path)\n");
        std::_Exit(2);
    }).detach();

    QElapsedTimer timer;

#ifdef Q_OS_MACOS
    qunsetenv("AETHER_ASR_FORCE_METAL");

    timer.start();
    const std::vector<AsrGpuDevice> gated = asrGpuDevices();
    const qint64 gatedMs = timer.elapsed();

    if (!hostIsAppleSilicon()) {
        if (!gated.empty()) {
            std::fprintf(stderr, "[FAIL] Intel Mac was offered %zu Metal "
                                 "device(s) without AETHER_ASR_FORCE_METAL\n",
                         gated.size());
            return 1;
        }
        if (gatedMs > 1000) {
            std::fprintf(stderr, "[FAIL] gated probe took %lld ms - the gate "
                                 "must answer before any ggml work\n",
                         static_cast<long long>(gatedMs));
            return 1;
        }
        std::printf("[ok] Intel Mac gate: no Metal offered, answered in "
                    "%lld ms\n", static_cast<long long>(gatedMs));
    } else {
        std::printf("[ok] Apple Silicon: %zu device(s) in %lld ms\n",
                    gated.size(), static_cast<long long>(gatedMs));
    }

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

    return 0;
}
