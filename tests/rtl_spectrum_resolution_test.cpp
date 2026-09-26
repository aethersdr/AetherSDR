#include "core/backends/rtl/RtlSdrDdc.h"
#include "core/backends/rtl/RtlViewport.h"
#include <QCoreApplication>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <random>

using AetherSDR::rtl::RtlSdrDdc;
namespace {
constexpr int kWindow = 65536;
constexpr double kRate = 2400000;
int failures = 0;
void check(bool ok, const char* message)
{
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
QVector<std::complex<float>> carriers(int count)
{
    QVector<std::complex<float>> iq(count);
    for (int n = 0; n < count; ++n) {
        const double t = n / kRate;
        const auto tone = [t](double hz, double amplitude) {
            return std::polar(amplitude, 2 * std::numbers::pi * hz * t);
        };
        iq[n] = static_cast<std::complex<float>>(tone(137500, .15) + tone(138000, .10));
    }
    return iq;
}
std::vector<float> bins(const QByteArray& frame)
{
    std::vector<float> result(frame.size() / sizeof(float));
    std::memcpy(result.data(), frame.constData(), frame.size());
    return result;
}
int peakNear(const std::vector<float>& p, double hz)
{
    const int expected = int(std::lround(kWindow / 2 + hz * kWindow / kRate));
    const int low = std::max(0, expected - 1), high = std::min(kWindow, expected + 2);
    return int(std::max_element(p.begin() + low, p.begin() + high) - p.begin());
}
std::vector<QByteArray> run(const QVector<std::complex<float>>& input, int partition, int fps = 30)
{
    RtlSdrDdc ddc;
    ddc.setSpectrumRateFps(fps);
    std::vector<QByteArray> frames;
    QObject::connect(&ddc, &RtlSdrDdc::spectrumFrameReady,
        [&](int, const QByteArray& frame) { frames.push_back(frame); });
    for (int first = 0; first < input.size(); first += partition) {
        ddc.processIqData(input.sliced(first, std::min(partition, int(input.size()) - first)), false);
    }
    return frames;
}
void continuityAndResolution()
{
    const auto iq = carriers(kWindow + 160000);
    RtlSdrDdc ddc;
    std::vector<QByteArray> frames;
    QObject::connect(&ddc, &RtlSdrDdc::spectrumFrameReady,
        [&](int, const QByteArray& frame) { frames.push_back(frame); });
    ddc.processIqData(iq.first(kWindow - 1), false);
    check(frames.empty(), "partial observation never emits zero-padded display FFT");
    ddc.processIqData(iq.sliced(kWindow - 1, 1), false);
    check(frames.size() == 1, "first complete continuous window emits exactly once");
    if (frames.empty() || frames.back().size() != kWindow * int(sizeof(float))) {
        check(false, "display supplies 65536 genuine bins"); return;
    }
    const auto p = bins(frames.back());
    for (double expected : {137500.0, 138000.0}) {
        const int peak = peakNear(p, expected);
        const double actual = (peak - kWindow / 2) * kRate / kWindow;
        check(std::abs(actual - expected) <= kRate / (2 * kWindow), "independent carrier maps within half a genuine bin");
    }
    const int left = peakNear(p, 137500), right = peakNear(p, 138000);
    const float valley = *std::min_element(p.begin() + left + 3, p.begin() + right - 2);
    check(valley < std::min(p[left], p[right]) - 20, "500 Hz separated carriers have a resolved valley");
    std::printf("resolution bin_hz=%.9f peaks_hz=%.9f,%.9f valley_db=%.3f\n", kRate/kWindow,
        (left-kWindow/2)*kRate/kWindow, (right-kWindow/2)*kRate/kWindow, valley);
    ddc.processIqData(iq.sliced(kWindow), false);
    check(frames.size() == 3, "30 FPS sample clock has exact 80000-sample cadence");
    check(run(iq, 257) == frames && run(iq, 8192) == frames,
        "variable USB partitioning preserves complete windows and frame cadence");
    check(run(iq, 8192, 60).size() == 5, "overlapping windows support 60 FPS without padding");
    check(run(iq, 8192, 1).size() == 1, "one FPS remains bounded independently of detector");

    // A changed capture invalidates the partially staged old RF observation.
    ddc.processIqData(iq.first(10000), false);
    ddc.applyCapture(kRate, 101000000, 101000000,
        AetherSDR::rtl::RtlCaptureTransaction::Mode::Fm, -8000, 8000);
    const auto before = frames.size();
    QVector<std::complex<float>> dc(kWindow, {0.1f, 0});
    ddc.processIqData(dc.first(kWindow - 1), false);
    check(frames.size() == before, "capture change requires a complete fresh observation");
    ddc.processIqData(dc.last(1), false);
    check(frames.size() == before + 1, "fresh capture fills and resumes");
    const auto after = bins(frames.back());
    check(after[peakNear(after, 137500)] < -100, "old carrier never leaks through capture reset");
    ddc.processIqData(iq.first(10000), false);
    ddc.resetSpectrum(); // Same-geometry USB restart / verified rollback.
    const auto beforeGap = frames.size();
    ddc.processIqData(dc.first(kWindow - 1), false);
    check(frames.size() == beforeGap, "same-geometry discontinuity also discards partial history");
    ddc.processIqData(dc.last(1), false);
    check(frames.size() == beforeGap + 1 && bins(frames.back())[peakNear(after,137500)] < -100,
        "gap recovery contains only the new continuous observation");

}
void mappingGainAndNoise()
{
    for (double hz : {-1000000.0, -250000.0, 0.0, 500000.0, 1000000.0}) {
        QVector<std::complex<float>> iq(kWindow);
        for (int i = 0; i < kWindow; ++i) {
            iq[i] = std::polar(0.2f, float(2 * std::numbers::pi * hz * i / kRate));
        }
        const auto frames = run(iq, 8192);
        if (frames.size() != 1 || frames[0].size() != kWindow * int(sizeof(float))) {
            check(false, "mapping probe gets one complete transform"); return;
        }
        const auto p = bins(frames[0]); const int peak = peakNear(p, hz);
        check(std::abs((peak-kWindow/2)*kRate/kWindow-hz) <= kRate/(2*kWindow),
            "negative, positive, edge and raw DC frequencies map correctly");
        if (hz == 0) {
            const double expected = 20 * std::log10(.2 * .35875 * (kWindow - 1) / kWindow);
            check(std::abs(p[peak] - expected) < .001, "coherent Blackman-Harris amplitude gain is preserved");
        }
        AetherSDR::SharedCapturePolicy::CaptureDescriptor capture{1, 1, 100000000, kRate, 1080000, 1080000};
        const auto view = AetherSDR::rtl::RtlViewport::fit(capture,kWindow,100000000+hz,18750);
        check(view && peak >= view->firstBin && peak < view->firstBin+view->binCount,
            "viewport edge mapping includes the independently placed RF carrier");
        if (view) {
            const double mapped = view->centerHz-view->spanHz/2+(peak-view->firstBin)*view->spanHz/view->binCount;
            check(std::abs(mapped-(100000000+hz)) <= kRate/(2*kWindow), "cropped bin axis preserves absolute RF");
        }
    }
    std::mt19937 rng(5468); std::normal_distribution<float> noise(0,.05f);
    QVector<std::complex<float>> iq(kWindow);
    double expectedPower = 0;
    for (int i = 0; i < kWindow; ++i) {
        iq[i] = {noise(rng),noise(rng)};
        const double a = 2*std::numbers::pi*i/(kWindow-1);
        const double w = .35875-.48829*std::cos(a)+.14128*std::cos(2*a)-.01168*std::cos(3*a);
        expectedPower += std::norm(iq[i])*w*w/kWindow;
    }
    const auto frames = run(iq,8192);
    if (frames.size() != 1 || frames[0].size() != kWindow*int(sizeof(float))) { return; }
    double measured = 0;
    for (float value : bins(frames[0])) { measured += std::pow(10.0,value/10.0); }
    check(std::abs(measured/expectedPower-1) < .001, "seeded noise follows window energy and FFT normalization");
    std::printf("noise_power measured=%.9g expected=%.9g\n",measured,expectedPower);
}
void detectorAndPerformance()
{
    RtlSdrDdc slow,fast;
    slow.setSpectrumRateFps(1); fast.setSpectrumRateFps(60);
    RtlSdrDdc detector;
    detector.processIqData(QVector<std::complex<float>>(8192, {0.1f,0}), false);
    const auto reference = detector.takeSquelchSpectrum();
    const double legacyGain = 20 * std::log10(.1 * .35875 * 2047 / 2048);
    check(reference.size() == 2048 && std::abs(reference[1024] - legacyGain) < .001,
        "squelch keeps its original absolute peak-bin normalization");
    const auto iq = carriers(2400000);
    int observations = 0;
    for (int first=0;first<iq.size();first+=8192) {
        const auto block=iq.sliced(first,std::min(8192,int(iq.size())-first));
        slow.processIqData(block,false); fast.processIqData(block,false);
        const auto a=slow.takeSquelchSpectrum(),b=fast.takeSquelchSpectrum();
        check(a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin()),
            "display rate does not alter established squelch samples or cadence");
        if (!a.empty()) { ++observations; check(a.size()==2048,"squelch retains 2048 measured bins"); }
    }
    check(observations>=29 && observations<=31,"detector retains its established nominal 30 Hz cadence");
    RtlSdrDdc bench; int frames=0;
    QObject::connect(&bench,&RtlSdrDdc::spectrumFrameReady,[&](int,const QByteArray&){++frames;});
    std::vector<QVector<std::complex<float>>> blocks;
    for(int first=0;first<iq.size();first+=8192) { blocks.push_back(iq.sliced(first,std::min(8192,int(iq.size())-first))); }
    const auto start=std::chrono::steady_clock::now();
    for(int repeat=0;repeat<10;++repeat) { for(const auto& block:blocks) { bench.processIqData(block,false); } }
    const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::printf("spectrum_benchmark iq_seconds=10 wall_seconds=%.6f frames=%d cpu_equivalent_percent=%.3f\n",seconds,frames,seconds*10);
}
}
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);
    continuityAndResolution(); mappingGainAndNoise(); detectorAndPerformance();
    return failures ? 1 : 0;
}
