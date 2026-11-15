// ClientLeveler, the AetherTX stage around WDSP's TX leveler (RFC #4836): a
// quiet voice is raised by Max gain and no more, a loud one is brought to
// the leveler's target, the delay equals the latency it reports (the attack
// sets the look-ahead), decay sets how fast gain recovers, every range
// clamps, and re-enabling starts clean. Socket-free.

#include "core/ClientLeveler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using AetherSDR::ClientLeveler;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;
int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-66s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

std::string fmt(const char* format, double a, double b = 0.0)
{
    char buf[160];
    std::snprintf(buf, sizeof buf, format, a, b);
    return buf;
}

std::unique_ptr<ClientLeveler> makeLeveler(float maxGainDb)
{
    auto lev = std::make_unique<ClientLeveler>();
    lev->setMaxGainDb(maxGainDb);
    lev->prepare(kRate);
    lev->setEnabled(true);
    return lev;
}

// Runs a 1 kHz tone of `amplitude` for `seconds` (mono, 480-frame blocks)
// and returns the output peak over the last quarter.
double steadyPeak(ClientLeveler& lev, double amplitude, double seconds = 2.0)
{
    const int total = static_cast<int>(seconds * kRate);
    std::vector<float> block(480);
    double peak = 0.0;
    for (int at = 0; at < total; at += 480) {
        for (int i = 0; i < 480; ++i) {
            block[static_cast<std::size_t>(i)] =
                static_cast<float>(amplitude * std::sin(2.0 * kPi * 1000.0 * (at + i) / kRate));
        }
        lev.process(block.data(), 480, 1);
        if (at >= total * 3 / 4) {
            for (float v : block) {
                peak = std::max(peak, std::fabs(static_cast<double>(v)));
            }
        }
    }
    return peak;
}

double db(double ratio)
{
    return 20.0 * std::log10(std::max(ratio, 1e-12));
}

void testDefaults()
{
    ClientLeveler lev;
    report("ships off", !lev.isEnabled());
    report("create_txa's leveler: 5 dB, 1 ms attack, 500 ms decay, no hang",
           lev.maxGainDb() == 5.0f && lev.attackMs() == 1.0f && lev.decayMs() == 500.0f
               && lev.hangMs() == 0.0f);
}

void testGain()
{
    auto quiet5 = makeLeveler(5.0f);
    const double g5 = db(steadyPeak(*quiet5, 0.05) / 0.05);
    report("a quiet voice is raised by Max gain", std::fabs(g5 - 5.0) < 0.3, fmt("%+.2f dB", g5));

    auto quiet20 = makeLeveler(20.0f);
    const double g20 = db(steadyPeak(*quiet20, 0.05) / 0.05);
    report("and by no more than Max gain", std::fabs(g20 - 20.0) < 0.3, fmt("%+.2f dB", g20));

    auto loud = makeLeveler(20.0f);
    const double out = steadyPeak(*loud, 0.5);
    report("a loud voice is brought to the leveler's target, not raised 20 dB",
           out > 0.95 && out < 1.1, fmt("peak %.3f from 0.5 in", out));
    report("the gain readout follows it", std::fabs(loud->gainDb() - db(out / 0.5)) < 0.5,
           fmt("readout %+.2f dB, measured %+.2f dB", loud->gainDb(), db(out / 0.5)));
}

int impulseDelay(ClientLeveler& lev)
{
    const int total = 8192;
    std::vector<float> x(total, 0.0f);
    x[100] = 0.01f;
    for (int at = 0; at < total; at += 480) {
        lev.process(x.data() + at, std::min(480, total - at), 1);
    }
    return static_cast<int>(std::max_element(x.begin(), x.end(),
                                             [](float a, float b) { return std::fabs(a) < std::fabs(b); })
                            - x.begin())
           - 100;
}

void testLatency()
{
    auto lev = makeLeveler(5.0f);
    const int d1 = impulseDelay(*lev);
    report("delay equals latencyFrames() at 1 ms attack", d1 == lev->latencyFrames(),
           fmt("delay %.0f, reported %.0f", d1, lev->latencyFrames()));
    auto slow = std::make_unique<ClientLeveler>();
    slow->setAttackMs(10.0f);
    slow->prepare(kRate);
    slow->setEnabled(true);
    const int d10 = impulseDelay(*slow);
    report("and at 10 ms attack, where the look-ahead is longer",
           d10 == slow->latencyFrames() && d10 > d1 + 2000,
           fmt("delay %.0f, reported %.0f", d10, slow->latencyFrames()));
}

// After a loud passage the gain climbs back toward Max gain at the decay rate.
double gainAfterLoud(float decayMs)
{
    auto lev = makeLeveler(20.0f);
    lev->setDecayMs(decayMs);
    steadyPeak(*lev, 0.5, 1.0);
    std::vector<float> quiet(480);
    for (int at = 0; at < static_cast<int>(0.3 * kRate); at += 480) {
        for (int i = 0; i < 480; ++i) {
            quiet[static_cast<std::size_t>(i)] =
                static_cast<float>(0.05 * std::sin(2.0 * kPi * 1000.0 * (at + i) / kRate));
        }
        lev->process(quiet.data(), 480, 1);
    }
    return lev->gainDb();
}

void testDecay()
{
    const double fast = gainAfterLoud(50.0f);
    const double slow = gainAfterLoud(2000.0f);
    report("a short decay recovers gain faster than a long one", fast > slow + 3.0,
           fmt("%+.1f dB after 300 ms at 50 ms decay, %+.1f dB at 2000 ms", fast, slow));
}

void testRangeLimits()
{
    ClientLeveler lev;
    lev.setMaxGainDb(30.0f);
    const float gHigh = lev.maxGainDb();
    lev.setMaxGainDb(-5.0f);
    report("Max gain clamps to 0..20 dB", gHigh == 20.0f && lev.maxGainDb() == 0.0f);
    lev.setAttackMs(0.2f);
    const float aLow = lev.attackMs();
    lev.setAttackMs(50.0f);
    report("attack clamps to 1..10 ms", aLow == 1.0f && lev.attackMs() == 10.0f);
    lev.setDecayMs(0.0f);
    const float dLow = lev.decayMs();
    lev.setDecayMs(9999.0f);
    report("decay clamps to 1..5000 ms", dLow == 1.0f && lev.decayMs() == 5000.0f);
    lev.setHangMs(5.0f);
    const float hOff = lev.hangMs();
    lev.setHangMs(9999.0f);
    report("hang below 10 ms is off, and clamps at 5000 ms",
           hOff == 0.0f && lev.hangMs() == 5000.0f);
    lev.setDecayMs(std::numeric_limits<float>::quiet_NaN());
    report("NaN falls back to the default", lev.decayMs() == ClientLeveler::kDefaultDecayMs);
}

void testReenableStartsClean()
{
    auto lev = makeLeveler(10.0f);
    steadyPeak(*lev, 0.5, 0.5);
    lev->setEnabled(false);
    lev->setEnabled(true);
    std::vector<float> silence(480, 0.0f);
    lev->process(silence.data(), 480, 1);
    report("re-enabling does not replay audio from the last run",
           std::all_of(silence.begin(), silence.end(), [](float v) { return v == 0.0f; }));
}

}  // namespace

int main()
{
    testDefaults();
    testGain();
    testLatency();
    testDecay();
    testRangeLimits();
    testReenableStartsClean();
    std::printf("%s\n", g_failed ? "FAILED" : "PASSED");
    return g_failed ? 1 : 0;
}
