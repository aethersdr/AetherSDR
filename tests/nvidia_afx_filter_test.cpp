// Functional test for the optional NVIDIA AFX GPU denoiser wrapper.
// CMake target `nvidia_afx_filter_test`. Exit 0 = pass (or SKIP).
//
// This exercises the in-app NvidiaAfxFilter end to end: it dlopens the AFX
// runtime from a pack, builds the TensorRT engine, and runs the denoiser on
// synthetic audio. It is hardware-gated: if no pack / no NVIDIA GPU is present
// (AETHER_NVAFX_DIR unset or the engine fails to load) the test SKIPs (exit 0)
// so it is harmless on CI runners without an NVIDIA card.
//
// Point it at a pack with:
//   AETHER_NVAFX_DIR=/path/to/Audio_Effects_SDK ./nvidia_afx_filter_test

#include "core/NvidiaAfxFilter.h"

#include <QByteArray>
#include <QByteArrayList>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using AetherSDR::NvidiaAfxFilter;

namespace {
int g_failed = 0;
void report(const char* name, bool ok, const char* detail = "")
{
    std::printf("%s %-60s%s\n", ok ? "[ OK ]" : "[FAIL]", name, detail);
    if (!ok) ++g_failed;
}

double rms(const QByteArray& pcmStereoFloat)
{
    const auto* s = reinterpret_cast<const float*>(pcmStereoFloat.constData());
    const int n = pcmStereoFloat.size() / static_cast<int>(sizeof(float));
    if (n == 0) return 0.0;
    double acc = 0.0;
    for (int i = 0; i < n; ++i) acc += double(s[i]) * s[i];
    return std::sqrt(acc / n);
}

bool allFinite(const QByteArray& pcmStereoFloat)
{
    const auto* s = reinterpret_cast<const float*>(pcmStereoFloat.constData());
    const int n = pcmStereoFloat.size() / static_cast<int>(sizeof(float));
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(s[i])) return false;
    return true;
}
} // namespace

int main()
{
    const char* dir = std::getenv("AETHER_NVAFX_DIR");
    NvidiaAfxFilter filter(dir ? QString::fromLocal8Bit(dir) : QString());

    if (!filter.isValid()) {
        std::printf("[SKIP] NVIDIA AFX denoiser not available: %s\n",
                    filter.lastError().toLocal8Bit().constData());
        std::printf("       (set AETHER_NVAFX_DIR to a pack on an NVIDIA GPU to run)\n");
        return 0;  // hardware-gated: not a failure
    }
    std::printf("AFX denoiser ready.\n");

    // 0.5 s of 24 kHz stereo float32 white noise (no speech). A speech denoiser
    // should attenuate pure noise substantially.
    const int rate = 24000, blockFrames = 240 /*10 ms*/;
    const int totalFrames = rate / 2;
    unsigned seed = 1234567u;
    auto noise = [&]() {
        seed = seed * 1103515245u + 12345u;
        return (float(int(seed >> 8 & 0xffff) - 32768) / 32768.0f) * 0.3f;
    };

    QByteArrayList outputs;
    QByteArray inAll, outAll;
    bool sizeOk = true, finiteOk = true;
    for (int produced = 0; produced < totalFrames; produced += blockFrames) {
        QByteArray in;
        in.resize(blockFrames * 2 * sizeof(float));
        auto* w = reinterpret_cast<float*>(in.data());
        for (int i = 0; i < blockFrames; ++i) { float v = noise(); w[2*i] = v; w[2*i+1] = v; }

        QByteArray out = filter.process(in);
        if (out.size() != in.size()) sizeOk = false;
        if (!allFinite(out)) finiteOk = false;
        inAll.append(in);
        outAll.append(out);
    }

    report("process() preserves block byte-count", sizeOk);
    report("output is all finite (no NaN/Inf)", finiteOk);

    // Skip the first 100 ms (engine priming / resampler warmup) for the RMS check.
    const int skipBytes = (rate / 10) * 2 * sizeof(float);
    const QByteArray inSteady  = inAll.mid(skipBytes);
    const QByteArray outSteady = outAll.mid(skipBytes);
    const double inR = rms(inSteady), outR = rms(outSteady);
    char detail[128];
    std::snprintf(detail, sizeof(detail), "  inRMS=%.4f outRMS=%.4f (%.0f%% reduction)",
                  inR, outR, inR > 0 ? (1.0 - outR / inR) * 100.0 : 0.0);
    report("denoiser attenuates pure noise (outRMS < 0.8*inRMS)",
           inR > 0 && outR < 0.8 * inR, detail);

    // Intensity setter must not crash and process must keep working.
    filter.setIntensity(0.5f);
    QByteArray probe(blockFrames * 2 * sizeof(float), '\0');
    report("setIntensity + process stable", filter.process(probe).size() == probe.size());

    std::printf(g_failed ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failed);
    return g_failed ? 1 : 0;
}
