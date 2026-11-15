#include "core/backends/rtl/RtlSquelchGate.h"
#include "gui/SpectrumSquelchLogic.h"
#include <array>
#include <cstdio>
#include <limits>
#include <random>

using Gate = AetherSDR::rtl::RtlSquelchGate;
int main()
{
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
    };
    Gate gate;
    gate.configure(true, 50, true); // -60 dBFS/bin
    check(gate.gain(0) == 0, "enabled detector starts closed until a measurement");
    gate.observe(-65, 0);
    check(gate.gain(1) == 0, "below manual threshold stays closed");
    gate.observe(-59, 100);
    float previous = 0;
    for (int n = 100; n <= 340; ++n) {
        const float value = gate.gain(n);
        check(value >= previous && value - previous <= Gate::kRampStep + 1e-6,
            "opening is bounded to a five-ms ramp");
        previous = value;
    }
    check(previous == 1, "strong signal opens fully");
    gate.observe(-62, 3000);
    check(gate.gain(3000) == 1, "three-dB hysteresis retains an open signal");
    gate.observe(-64, 6000);
    check(gate.gain(9000) == 1, "short fades retain audio through hang time");
    gate.observe(-64, 10000);
    for (int n = 10001; n < 10500; ++n) { gate.gain(n); }
    check(gate.gain(10500) == 0, "sustained fade closes after hang and ramp");
    gate.observe(-50, 11000);
    for (int n = 11000; n < 11500; ++n) { gate.gain(n); }
    gate.configure(true, 80); // Auto/manual threshold updates do not rebuild DSP
    gate.observe(-50, 12000);
    gate.observe(-50, 16000);
    for (int n = 18201; n < 18700; ++n) { gate.gain(n); }
    check(gate.gain(18700) == 0, "higher threshold closes the existing detector");
    gate.configure(false, 80);
    for (int n = 18701; n < 19000; ++n) { gate.gain(n); }
    check(gate.gain(19000) == 1, "Off passes audio without detector input");
    gate.configure(true, 50, true);
    gate.observe(-40, 20000);
    for (int n = 20000; n < 20300; ++n) { gate.gain(n); }
    for (int n = 24801; n < 25200; ++n) { gate.gain(n); }
    check(gate.gain(25200) == 0, "missing spectra close instead of holding stale signal");
    gate.observe(std::numeric_limits<double>::quiet_NaN(), 25300);
    check(gate.gain(25300) == 0, "invalid detector input stays closed");

    std::array<float, 2048> bins;
    bins.fill(-80);
    bins[100] = -25; // a signal must not raise the trimmed noise estimate
    float floor = -999;
    auto level = AetherSDR::SpectrumSquelchLogic::suggest(bins, floor,
        Gate::kReferenceDb, Gate::kStepDb, 10);
    check(level && *level == 42 && floor == -80, "Auto encodes -70 dBFS/bin as level 42, not Flex level 90");
    gate.configure(true, *level, true);
    gate.observe(-80, 0);
    check(gate.gain(1) == 0, "Auto closes on its measured floor");
    gate.observe(-65, 100);
    for (int n = 100; n < 400; ++n) { gate.gain(n); }
    check(gate.gain(400) == 1, "Auto opens a signal above floor plus margin");
    bins.fill(-60);
    for (int n = 0; n < 60; ++n) {
        level = AetherSDR::SpectrumSquelchLogic::suggest(bins, floor,
            Gate::kReferenceDb, Gate::kStepDb, 10);
    }
    check(level && *level == 58, "Auto follows a rising noise floor in detector units");
    floor = -999; bins.fill(-100);
    level = AetherSDR::SpectrumSquelchLogic::suggest(bins, floor, -160, 1, 10);
    check(level && *level == 70, "legacy Flex Auto scale is preserved");
    bins.fill(std::numeric_limits<float>::quiet_NaN());
    check(!AetherSDR::SpectrumSquelchLogic::suggest(bins, floor, -120, 1.2, 10),
        "invalid spectra produce no Auto command");
    // Auto belongs to each acquisition gate and consumes its detector bins,
    // independent of display FFT size, smoothing, visibility or selected slice.
    std::array<Gate, 8> receivers;
    for (auto& receiver : receivers) { receiver.configure(true, 20, true, true, 10); }
    bins.fill(-60);
    for (int n = 0; n < 100; ++n) {
        const auto frame = static_cast<std::uint64_t>(n * 1600);
        for (auto& receiver : receivers) {
            receiver.observeSpectrum(bins, 1000, 1012, frame);
            for (int i = 0; i < 1600; ++i) { receiver.gain(frame + i); }
            check(receiver.gain(frame + 1599) == 0, "Auto closes detector noise in all eight slots");
        }
    }
    bins[1006] = -40;
    receivers[7].observeSpectrum(bins, 1000, 1012, 160000);
    for (int i = 0; i < 300; ++i) { receivers[7].gain(160000 + i); }
    check(receivers[7].gain(160300) == 1, "Auto opens signal above local floor plus margin");
    check(receivers[0].gain(160300) == 0, "one receiver signal does not open another");
    bins.fill(-40); // sustained rise must be learned from the same detector
    for (int n = 101; n < 201; ++n) {
        receivers[7].observeSpectrum(bins, 1000, 1012, n * 1600);
        for (int i = 0; i < 1600; ++i) { receivers[7].gain(n * 1600 + i); }
    }
    check(receivers[7].gain(321599) == 0, "Auto adapts to a rising noise floor");
    receivers[7].configure(true, 80, false, false, 10);
    receivers[7].observeSpectrum(bins, 1000, 1012, 322000);
    for (int i = 0; i < 300; ++i) { receivers[7].gain(322000 + i); }
    check(receivers[7].gain(322300) == 0, "manual threshold replaces Auto without stale automatic state");
    Gate noisy;
    noisy.configure(true, 20, true, true, 10);
    std::mt19937 random(5468);
    std::exponential_distribution<double> noisePower(1.0);
    int quietFrames = 0;
    for (int n = 0; n < 300; ++n) {
        // Complex Gaussian receiver noise has exponential FFT-bin power.
        // A trimmed log mean underestimates that floor and holds noise open.
        for (auto& bin : bins) { bin = -60 + 10 * std::log10(noisePower(random)); }
        noisy.observeSpectrum(bins, 1000, 1012, n * 1600);
        for (int i = 0; i < 1600; ++i) { noisy.gain(n * 1600 + i); }
        quietFrames += noisy.gain(n * 1600 + 1599) == 0;
    }
    check(quietFrames >= 285, "default Auto margin rejects at least 95 percent of noise-only frames");
    return failures ? 1 : 0;
}
