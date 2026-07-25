// aetherd HL2 Phase 1a — Hl2RxDsp integration test. Pushes a synthetic +1 kHz
// IQ tone through the full RX DSP as 126-sample EP6-shaped blocks and checks the
// three outputs wire up: the panadapter peaks on the positive-frequency side
// (not DC), the WdspChannel demod produces non-silent audio at the right block
// size, and the S-meter tracks it. Exercises the 126 -> 1024 block buffering.

#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisProtocol.h"   // kSamplesPerPacket (EP6 block size)

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <span>
#include <vector>

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static constexpr double kPi = 3.14159265358979323846;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    Hl2RxDsp dsp;
    Hl2RxDsp::Config cfg;
    cfg.inputSampleRateHz = 48000;
    cfg.audioSampleRateHz = 48000;
    cfg.dspBlockSize = 1024;
    cfg.fftSize = 256;
    cfg.mode = WdspChannel::Mode::Usb;
    cfg.blockForOutput = true;   // deterministic audio for this offline burst feed
    std::string err;

    // A zero input sample rate — as a missing/malformed "sampleRateHz" params
    // override decodes to (QVariant::toInt) — must be rejected at the boundary,
    // not divide-by-zero in the dsp_size computation (#4448). Guard runs before
    // any state is touched, so the good configure below still succeeds.
    {
        Hl2RxDsp guardDsp;
        Hl2RxDsp::Config bad = cfg;
        bad.inputSampleRateHz = 0;
        std::string guardErr;
        check(!guardDsp.configure(bad, &guardErr),
              "zero inputSampleRateHz is rejected, not a divide-by-zero");
        check(!guardErr.empty(), "rejected configure reports an error string");
    }

    check(dsp.configure(cfg, &err), err.empty() ? "Hl2RxDsp configures" : err.c_str());

    int audioCount = 0, specCount = 0;
    std::size_t lastAudioSize = 0;
    float maxMeter = -300.0f;
    std::vector<float> lastBins;
    QObject::connect(&dsp, &Hl2RxDsp::audioReady, &dsp,
                     [&](const std::vector<float>& pcm) { ++audioCount; lastAudioSize = pcm.size(); });
    float lastMeter = -300.0f;
    QObject::connect(&dsp, &Hl2RxDsp::meterUpdate, &dsp,
                     [&](float dbfs) { if (dbfs > maxMeter) maxMeter = dbfs; lastMeter = dbfs; });
    QObject::connect(&dsp, &Hl2RxDsp::spectrumReady, &dsp,
                     [&](const std::vector<float>& bins) { ++specCount; lastBins = bins; });

    // Feed a +1 kHz complex tone (in the USB passband) as 126-sample blocks.
    const int fs = 48000;
    const double f = 1000.0;
    const int total = fs / 2;                 // ~0.5 s
    std::vector<std::complex<float>> stream(static_cast<std::size_t>(total));
    for (int n = 0; n < total; ++n) {
        const double ph = 2.0 * kPi * f * n / fs;
        stream[static_cast<std::size_t>(n)] =
            0.3f * std::complex<float>(static_cast<float>(std::cos(ph)),
                                       static_cast<float>(std::sin(ph)));
    }
    std::span<const std::complex<float>> s(stream);
    for (std::size_t off = 0; off < s.size(); off += kSamplesPerPacket) {
        const std::size_t n = std::min<std::size_t>(kSamplesPerPacket, s.size() - off);
        const auto sub = s.subspan(off, n);
        dsp.processIqBlock(std::vector<std::complex<float>>(sub.begin(), sub.end()));
    }

    // ---- panadapter ----
    check(specCount > 0, "spectrumReady fired");
    check(lastBins.size() == 256, "spectrum has fftSize bins");
    if (lastBins.size() == 256) {
        int peak = 0;
        for (int i = 1; i < 256; ++i)
            if (lastBins[static_cast<std::size_t>(i)] > lastBins[static_cast<std::size_t>(peak)]) peak = i;
        check(peak > 128, "+1 kHz tone peaks on the positive-frequency side (above centre)");
        check(lastBins[static_cast<std::size_t>(peak)] > lastBins[128] + 15.0f,
              "tone peak stands well above the DC (centre) bin");
    }

    // ---- audio ----
    check(audioCount >= 5, "audioReady fired for the buffered DSP blocks");
    check(lastAudioSize == static_cast<std::size_t>(cfg.dspBlockSize) * 2,
          "audio block is interleaved stereo of outputBlockSize");
    check(maxMeter > -60.0f, "demod produced non-silent audio (S-meter above floor)");

    // ---- the S-meter must track SIGNAL STRENGTH, not the AGC's output ----
    //
    // This is the assertion an audio-RMS meter cannot pass. Holding the audio
    // level constant is exactly what the AGC does, so a meter derived from
    // demodulated audio barely moves between a strong signal and a weak one —
    // it deflects, which is why it looked like it worked. Feed the same tone
    // 40 dB down and require the meter to follow it down.
    {
        // RXA_S_PK is a PEAK detector with decay, so compare settled values —
        // the maximum during the weak run is just the decay from the strong one.
        const float strongMeter = lastMeter;
        const double weakAmp = 0.3 * 0.01;          // -40 dB
        for (int n = 0; n < total; ++n) {
            const double ph = 2.0 * kPi * f * n / fs;
            stream[static_cast<std::size_t>(n)] =
                std::complex<float>(static_cast<float>(weakAmp * std::cos(ph)),
                                    static_cast<float>(weakAmp * std::sin(ph)));
        }
        // Feed several seconds: RXA_S_PK decays rather than jumping, so a short
        // burst measures the decay slope instead of the settled level.
        std::span<const std::complex<float>> w(stream);
        for (int pass = 0; pass < 6; ++pass) {
            for (std::size_t off = 0; off < w.size(); off += kSamplesPerPacket) {
                const std::size_t n = std::min<std::size_t>(kSamplesPerPacket, w.size() - off);
                const auto sub = w.subspan(off, n);
                dsp.processIqBlock(std::vector<std::complex<float>>(sub.begin(), sub.end()));
            }
        }
        const float drop = strongMeter - lastMeter;
        std::fprintf(stderr, "S-meter: strong %.1f, weak %.1f, drop %.1f dB (input -40 dB)\n",
                     strongMeter, lastMeter, drop);
        // A 40 dB input drop must move the meter by at least 30 dB. An
        // audio-RMS meter under AGC moves by ~0 dB, which is the point.
        check(drop > 30.0f,
              "S-meter follows a 40 dB signal drop (audio-RMS meter would not)");
    }

    // ---- mode change doesn't break the pipeline ----
    const int before = audioCount;
    dsp.setMode(WdspChannel::Mode::Am);
    for (int b = 0; b < 40; ++b) {
        std::vector<std::complex<float>> blk(kSamplesPerPacket, std::complex<float>(0.2f, 0.0f));
        dsp.processIqBlock(blk);
    }
    check(audioCount > before, "audio continues after a mode change");

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_rxdsp_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
