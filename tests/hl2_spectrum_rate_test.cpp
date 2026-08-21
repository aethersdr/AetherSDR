// The panadapter frame rate must follow the OPERATOR'S SLIDER, not the span.
//
// That decoupling is the whole point of the display-rate shaper: without it the
// rate defaults to the IQ sample rate over the FFT size, so zooming out speeds
// the display up and zooming in slows it down. docs/HERMES.md documents the shaper as
// delivering a span-independent rate, but the first implementation emptied the
// FFT accumulator whenever an interval was skipped — and refilling it costs
// ~fftSize/126 EP6 blocks, which is 23.6 ms at 48 kHz against 3.0 ms at 384 kHz.
// Added to the interval, a 25 fps request landed near 16 fps zoomed in and 23 fps
// zoomed out: the rate tracked the span after all, just less obviously.
//
// This measures the achieved rate at every rate the gateware offers, by feeding
// wall-clock-paced EP6-shaped blocks and counting the spectra that come out.
#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisProtocol.h"   // kEp6BlockSamples

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
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

// Feed `seconds` of IQ at `rateHz` in 126-sample EP6 blocks, paced against the
// wall clock so the shaper's real QElapsedTimer gating is what decides, and
// return the number of spectra emitted.
static int spectraIn(int rateHz, int requestedFps, double seconds,
                     std::string* err)
{
    Hl2RxDsp dsp;
    Hl2RxDsp::Config cfg;
    cfg.inputSampleRateHz = rateHz;
    cfg.audioSampleRateHz = 24000;
    cfg.dspBlockSize = 1024;
    cfg.fftSize = 1024;
    cfg.mode = WdspChannel::Mode::Usb;
    cfg.filterLowHz = 150.0;
    cfg.filterHighHz = 3000.0;
    if (!dsp.configure(cfg, err))
        return -1;
    dsp.setSpectrumRateFps(requestedFps);

    int frames = 0;
    QObject::connect(&dsp, &Hl2RxDsp::spectrumReady, &dsp,
                     [&frames](const std::vector<float>&) { ++frames; });

    // Deliver blocks at the rate real hardware would, using the wall clock the
    // shaper itself reads. Batch per 5 ms tick so the loop is affordable at
    // 384 kHz (3048 blocks/second).
    const double blocksPerSecond =
        static_cast<double>(rateHz) / static_cast<double>(kEp6BlockSamples);
    QElapsedTimer clock;
    clock.start();
    double delivered = 0.0;
    std::vector<std::complex<float>> blk(kEp6BlockSamples);
    long long n = 0;
    while (clock.elapsed() < static_cast<qint64>(seconds * 1000.0)) {
        const double due = blocksPerSecond * (clock.elapsed() / 1000.0);
        while (delivered < due) {
            for (auto& s : blk) {
                const double ph = 2.0 * 3.14159265358979323846 * 1000.0
                                * static_cast<double>(n++) / rateHz;
                s = {0.3f * static_cast<float>(std::cos(ph)),
                     0.3f * static_cast<float>(std::sin(ph))};
            }
            dsp.processIqBlock(blk);
            delivered += 1.0;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
    }
    return frames;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    constexpr int kRequestedFps = 25;
    constexpr double kSeconds = 1.0;
    const int rates[] = {48000, 96000, 192000, 384000};

    struct Row { int rateHz; double fps; };
    std::vector<Row> rows;

    for (const int rate : rates) {
        std::string err;
        const int frames = spectraIn(rate, kRequestedFps, kSeconds, &err);
        if (frames < 0) {
            std::fprintf(stderr, "FAIL: %d Hz did not configure: %s\n",
                         rate, err.c_str());
            ++g_failures;
            continue;
        }
        const double fps = frames / kSeconds;
        rows.push_back({rate, fps});
        std::fprintf(stderr, "%6d Hz span -> %.1f fps (requested %d)\n",
                     rate, fps, kRequestedFps);
        // Generous floor: this runs on shared CI, so the point is not the exact
        // number but that a NARROW span is not starved. The pre-fix code landed
        // near 16 fps here, i.e. below 0.75 of the request.
        check(fps >= kRequestedFps * 0.75,
              "the requested frame rate is approximately achieved at this span");
        // And never faster than asked: the cap is a cap.
        check(fps <= kRequestedFps * 1.25,
              "the shaper does not exceed the requested frame rate");
    }

    // THE assertion. Whatever the absolute numbers do on a loaded machine, the
    // narrow span must not run materially slower than the wide one — that spread
    // IS the coupling this shaper removes.
    if (rows.size() >= 2) {
        double lo = 1e9, hi = 0.0;
        for (const Row& r : rows) {
            lo = std::min(lo, r.fps);
            hi = std::max(hi, r.fps);
        }
        const double spread = hi > 0.0 ? (hi - lo) / hi : 0.0;
        std::fprintf(stderr, "frame-rate spread across spans: %.1f%%\n",
                     spread * 100.0);
        check(spread < 0.25,
              "the achieved frame rate does not track the span");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_spectrum_rate_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
