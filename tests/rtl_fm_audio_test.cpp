#include "core/dsp/WdspChannel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <random>

namespace {
int failures = 0;
void check(bool result, const char* message)
{
    if (!result) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

double measure(double deviation, double tone, double amplitude)
{
    constexpr int kRate = 48000;
    constexpr int kBlock = 1024;
    constexpr int kDiscard = 98304;
    constexpr int kCount = 48000;
    WdspChannel::Config config;
    config.mode = WdspChannel::Mode::Fm;
    config.fmReceive = WdspChannel::FmReceive{};
    config.fmDeviationHz = deviation;
    config.filterHighHz = deviation == 5000 ? 12000 : 8000;
    config.filterLowHz = -config.filterHighHz;
    config.blockForOutput = true;
    auto channel = WdspChannel::create(config);
    check(channel != nullptr, "FM recipe constructs");
    if (!channel) { return 0; }
    std::array<float, kBlock> inI{}, inQ{}, left{}, right{};
    double peak = 0, allPeak = 0, sum = 0, energy = 0, re = 0, im = 0;
    int measured = 0;
    for (int first = 0; measured < kCount; first += kBlock) {
        for (int i = 0; i < kBlock; ++i) {
            const double phase = deviation / tone
                * std::sin(2 * std::numbers::pi * tone * (first + i) / kRate);
            inI[i] = amplitude * std::cos(phase);
            inQ[i] = amplitude * std::sin(phase);
        }
        check(channel->processIq(inI, inQ, left, right) == WdspChannel::ProcessResult::Ok,
            "FM IQ processes without allocation or underrun");
        for (float value : left) {
            check(std::isfinite(value), "FM PCM is finite");
            allPeak = std::max(allPeak, std::abs(double(value)));
            if (first < kDiscard || measured == kCount) { continue; }
            const double phase = 2 * std::numbers::pi * tone * measured / kRate;
            peak = std::max(peak, std::abs(double(value)));
            sum += value; energy += double(value) * value;
            re += value * std::cos(phase); im += value * std::sin(phase);
            ++measured;
        }
    }
    const double rms = std::sqrt(energy / kCount);
    const double fundamental = 2 * std::hypot(re, im) / kCount;
    const double residual = std::sqrt(std::max(0.0, energy / kCount
        - fundamental * fundamental / 2 - std::pow(sum / kCount, 2)));
    std::printf("FM deviation=%.0f tone=%.0f iq=%.2f peak=%.6f all_peak=%.6f rms=%.6f residual=%.6f\n",
        deviation, tone, amplitude, peak, allPeak, rms, residual / std::max(rms, 1e-12));
    check(peak < 0.95 && rms > 0.1, "settled full-deviation PCM has useful level and headroom");
    check(allPeak < 1.0, "startup does not hard-clip a full-deviation tone");
    check(residual / std::max(rms, 1e-12) < 0.005, "settled FM residual below 0.5 percent");
    check(!channel->setMode(WdspChannel::Mode::Usb), "FM normalization cannot leak into another mode");
    check(!channel->setFmDeviation(kRate / 2.0), "FM recipe runtime setter preserves Nyquist bound");
    check(channel->setFmDeviation(deviation), "FM recipe accepts its canonical runtime deviation");
    check(channel->config().fmDeviationHz == deviation, "runtime deviation has a single config owner");
    return fundamental;
}

void noiseTransitions()
{
    WdspChannel::Config config;
    config.mode = WdspChannel::Mode::Fm;
    config.fmReceive = WdspChannel::FmReceive{};
    config.fmDeviationHz = 2500;
    config.filterLowHz = -8000; config.filterHighHz = 8000;
    config.blockForOutput = true;
    auto channel = WdspChannel::create(config);
    check(channel != nullptr, "noise test constructs");
    if (!channel) { return; }
    std::mt19937 random(5468);
    std::uniform_real_distribution<float> noise(-0.3f, 0.3f);
    std::array<float, 1024> inI{}, inQ{}, left{}, right{};
    double peak = 0;
    for (int block = 0; block < 300; ++block) {
        // Deterministic silence/noise/carrier transitions, including startup.
        const int stage = (block / 30) % 3;
        for (int i = 0; i < 1024; ++i) {
            inI[i] = stage == 0 ? 0.0f : stage == 1 ? noise(random) : 0.3f;
            inQ[i] = stage == 1 ? noise(random) : 0.0f;
        }
        check(channel->processIq(inI, inQ, left, right) == WdspChannel::ProcessResult::Ok,
            "noise transitions process");
        for (float value : left) {
            check(std::isfinite(value), "noise PCM is finite");
            peak = std::max(peak, std::abs(double(value)));
        }
    }
    std::printf("FM noise/silence/carrier peak=%.6f\n", peak);
    check(peak < 1.0, "noise and carrier transitions retain normalized PCM headroom");
}
// HL2 can move from USB into FM without rebuilding its WDSP channel. Its
// FM detector must stay bounded even without the RTL-only FmReceive recipe.
void liveModeFmLimiter()
{
    WdspChannel::Config config;
    config.mode = WdspChannel::Mode::Usb;
    config.fmDetectorLimiter = true;
    config.rxPanelGain = 1.0;
    config.fmDeviationHz = 5000;
    config.filterLowHz = -8000;
    config.filterHighHz = 8000;
    config.blockForOutput = true;
    auto channel = WdspChannel::create(config);
    check(channel != nullptr, "mode-independent FM limiter constructs in USB");
    if (!channel) { return; }

    std::mt19937 random(18765);
    std::uniform_real_distribution<float> noise(-0.3f, 0.3f);
    std::array<float, 1024> inI{}, inQ{}, left{}, right{};
    double peak = 0;
    bool allFinite = true;
    bool allProcessed = true;

    auto process = [&](bool measurePeak) {
        for (int i = 0; i < 1024; ++i) {
            inI[i] = noise(random);
            inQ[i] = noise(random);
        }
        if (channel->processIq(inI, inQ, left, right) != WdspChannel::ProcessResult::Ok) {
            allProcessed = false;
            return;
        }
        if (measurePeak) {
            for (const float sample : left) {
                allFinite &= std::isfinite(sample);
                peak = std::max(peak, std::abs(double(sample)));
            }
        }
    };

    check(channel->setMode(WdspChannel::Mode::Fm), "enter FM without channel rebuild");
    for (int block = 0; block < 150; ++block) { process(true); }
    check(channel->setMode(WdspChannel::Mode::Usb), "leave FM with limiter armed");
    for (int block = 0; block < 10; ++block) { process(false); }
    check(channel->setMode(WdspChannel::Mode::Fm), "re-enter FM without channel rebuild");
    for (int block = 0; block < 150; ++block) { process(true); }
    std::printf("FM live mode-switch noise peak=%.6f\n", peak);
    check(allProcessed && allFinite, "FM live mode-switch keeps producing finite PCM");
    check(peak > 0.1 && peak < 1.0, "HL2 FM noise stays below full scale");
}

} // namespace

int main()
{
    WdspChannel::Config config;
    config.fmReceive = WdspChannel::FmReceive{};
    config.fmDeviationHz = 2500;
    check(!WdspChannel::create(config), "FM recipe rejects non-FM channel");
    config.mode = WdspChannel::Mode::Fm;
    for (double invalid : {0.0, -1.0, 24000.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
        config.fmDeviationHz = invalid;
        check(!WdspChannel::create(config), "FM recipe rejects invalid deviation");
    }
    for (double tone : {300.0, 1000.0, 2500.0}) {
        for (double amplitude : {0.03, 0.3}) {
            const double narrow = measure(2500.0, tone, amplitude);
            const double wide = measure(5000.0, tone, amplitude);
            check(narrow > 0.85 * wide && narrow < 1.15 * wide,
                "FMN and FM full-deviation tones retain the same normalization");
        }
    }
    noiseTransitions();
    liveModeFmLimiter();
    return failures ? 1 : 0;
}
