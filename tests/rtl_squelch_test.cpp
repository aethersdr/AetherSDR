#include "core/backends/rtl/RtlSquelchGate.h"
#include "gui/SpectrumSquelchLogic.h"
#include <array>
#include <cstdio>
#include <limits>

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
    return failures ? 1 : 0;
}
