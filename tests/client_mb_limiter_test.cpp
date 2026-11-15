// ClientMbLimiter, the AetherTX four-band look-ahead limiter (RFC #4836):
// below threshold the four bands sum flat; the delay is exactly the look-ahead
// it reports; a band is held at its threshold from the very first sample of a
// burst (true look-ahead) while the others pass; Release sets the recovery;
// a crossover change while running does not click; every range clamps and the
// crossovers stay ordered; re-enabling starts clean; the settings JSON
// round-trips. Socket-free.

#include "core/ClientMbLimiter.h"
#include "core/ClientMbLimiterJson.h"

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using AetherSDR::ClientMbLimiter;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;
constexpr int kChunk = 480;
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

double db(double ratio)
{
    return 20.0 * std::log10(std::max(ratio, 1e-12));
}

std::unique_ptr<ClientMbLimiter> makeLimiter(float thresholdDb)
{
    auto l = std::make_unique<ClientMbLimiter>();
    for (int b = 0; b < ClientMbLimiter::kBands; ++b) {
        l->setThresholdDb(b, thresholdDb);
    }
    l->prepare(kRate);
    l->setEnabled(true);
    return l;
}

void run(ClientMbLimiter& l, std::vector<float>& audio)
{
    for (std::size_t at = 0; at < audio.size(); at += kChunk) {
        const int frames = static_cast<int>(std::min<std::size_t>(kChunk, audio.size() - at));
        l.process(audio.data() + at, frames, 1);
    }
}

std::vector<float> tone(double hz, double seconds, double amplitude)
{
    std::vector<float> out(static_cast<std::size_t>(seconds * kRate));
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * i / kRate));
    }
    return out;
}

double rmsOf(const std::vector<float>& v, std::size_t from, std::size_t to)
{
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        sum += static_cast<double>(v[i]) * v[i];
    }
    return std::sqrt(sum / static_cast<double>(to - from));
}

double peakOf(const std::vector<float>& v, std::size_t from, std::size_t to)
{
    double p = 0.0;
    for (std::size_t i = from; i < std::min(to, v.size()); ++i) {
        p = std::max(p, std::fabs(static_cast<double>(v[i])));
    }
    return p;
}

void testDefaults()
{
    ClientMbLimiter l;
    report("ships off", !l.isEnabled());
    report("defaults: crossovers 250 / 1000 / 3000 Hz",
           l.crossoverHz(0) == 250.0f && l.crossoverHz(1) == 1000.0f && l.crossoverHz(2) == 3000.0f);
    bool bands = true;
    for (int b = 0; b < ClientMbLimiter::kBands; ++b) {
        bands = bands && l.thresholdDb(b) == -6.0f && l.releaseMs(b) == 100.0f;
    }
    report("defaults: every band -6 dBFS, 100 ms release", bands);
    report("latency is the 5 ms look-ahead", l.latencyFrames() == 240);
}

void testFlatSum()
{
    double worst = 0.0;
    for (double hz : {60.0, 150.0, 250.0, 500.0, 1000.0, 2000.0, 3000.0, 5000.0, 8000.0, 12000.0}) {
        auto l = makeLimiter(0.0f);
        auto audio = tone(hz, 1.0, 0.1);
        run(*l, audio);
        // RMS, not sample peak: at 12 kHz a sample peak can miss the true
        // peak by 3 dB depending on phase, and the all-pass moves the phase.
        const double level = db(rmsOf(audio, audio.size() / 2, audio.size()) / (0.1 / std::sqrt(2.0)));
        worst = std::max(worst, std::fabs(level));
    }
    report("below threshold the four bands sum flat, 60 Hz to 12 kHz", worst < 0.1,
           fmt("worst %.3f dB", worst));
}

void testLatency()
{
    auto l = makeLimiter(0.0f);
    std::vector<float> audio(2000, 0.0f);
    audio[0] = 0.1f;
    run(*l, audio);
    const bool silentBefore = peakOf(audio, 0, 240) == 0.0;
    report("nothing comes out before the look-ahead", silentBefore);
    report("the impulse starts exactly at the reported latency", std::fabs(audio[240]) > 1e-4,
           fmt("%.2e at frame 240", audio[240]));
}

void testBandLimiterNeverExceedsItsThreshold()
{
    // A band's limiter on its own: quiet, then bursts that jump to full level
    // with no warning, a single-sample spike, and a release back down.
    ClientMbLimiter::BandLimiter band;
    band.clear();
    const float threshold = 0.25f;
    band.configure(threshold, 50.0f, kRate);
    double worst = 0.0;
    double lastOut = 0.0;
    uint32_t seed = 12345;
    for (int64_t n = 0; n < 96000; ++n) {
        seed = seed * 1664525u + 1013904223u;
        const double noise = (static_cast<double>(seed >> 8) / 16777216.0 - 0.5) * 2.0;
        double x = 0.02 * noise;
        if ((n / 4800) % 3 == 1) {
            x = 0.9 * std::sin(2.0 * kPi * 300.0 * n / kRate) + 0.05 * noise;
        }
        if (n == 70000) {
            x = 1.0;
        }
        lastOut = band.process(static_cast<float>(x), n);
        worst = std::max(worst, std::fabs(lastOut));
    }
    report("a band's limiter never lets a sample past its threshold", worst <= threshold * (1.0 + 1e-5),
           fmt("worst %.5f vs %.5f", worst, threshold));
    (void)lastOut;
}

void testLookaheadHoldsTheBand()
{
    // Crossovers 80 / 2000 Hz put a 500 Hz tone almost wholly in band 2.
    auto l = std::make_unique<ClientMbLimiter>();
    l->setCrossoversHz(80.0f, 2000.0f, 4000.0f);
    for (int b = 0; b < ClientMbLimiter::kBands; ++b) {
        l->setThresholdDb(b, 0.0f);
    }
    l->setThresholdDb(1, -12.0f);
    l->prepare(kRate);
    l->setEnabled(true);
    // Quiet, then a burst that starts at full level with no warning.
    auto audio = tone(500.0, 1.0, 0.05);
    const std::size_t onset = audio.size() / 2;
    for (std::size_t i = onset; i < audio.size(); ++i) {
        audio[i] *= 18.0f;  // 0.9
    }
    run(*l, audio);
    const double ceiling = std::pow(10.0, -12.0 / 20.0);
    const double peak = peakOf(audio, onset, audio.size());
    // The other bands are not limited and carry the step's splatter and a
    // little leakage, so the sum may sit a little over band 2's threshold.
    report("a burst is held near its band threshold from the start", peak <= ceiling * 1.2,
           fmt("peak %.4f vs %.4f", peak, ceiling));
    const double steady = peakOf(audio, audio.size() - 4800, audio.size());
    report("and settles on it", steady <= ceiling * 1.03, fmt("steady %.4f", steady));
    report("and the gain reduction reads about 11 dB",
           l->takeGainReductionDb(1) < -10.0f && l->takeGainReductionDb(1) == 0.0f,
           "(and resets once read)");
    report("other bands are not reduced", l->takeGainReductionDb(0) > -0.5f
                                              && l->takeGainReductionDb(3) > -0.5f);

    // The same tone with band 2 at 0 dB passes at full level.
    auto open = std::make_unique<ClientMbLimiter>();
    open->setCrossoversHz(80.0f, 2000.0f, 4000.0f);
    for (int b = 0; b < ClientMbLimiter::kBands; ++b) {
        open->setThresholdDb(b, 0.0f);
    }
    open->prepare(kRate);
    open->setEnabled(true);
    auto loud = tone(500.0, 0.5, 0.9);
    run(*open, loud);
    report("a band at 0 dBFS passes a 0.9 tone untouched",
           std::fabs(db(peakOf(loud, loud.size() / 2, loud.size()) / 0.9)) < 0.1);
}

double recoveryMs(float releaseMs)
{
    auto l = std::make_unique<ClientMbLimiter>();
    l->setCrossoversHz(80.0f, 2000.0f, 4000.0f);
    for (int b = 0; b < ClientMbLimiter::kBands; ++b) {
        l->setThresholdDb(b, 0.0f);
        l->setReleaseMs(b, releaseMs);
    }
    l->setThresholdDb(1, -12.0f);
    l->prepare(kRate);
    l->setEnabled(true);
    auto audio = tone(500.0, 3.0, 0.9);
    const std::size_t drop = static_cast<std::size_t>(kRate);
    for (std::size_t i = drop; i < audio.size(); ++i) {
        audio[i] /= 18.0f;  // 0.05, well under threshold
    }
    run(*l, audio);
    // First 10 ms window, after the loud tail has left the delay, whose
    // peak is back within 1 dB.
    const std::size_t start = drop + 240 + 480;
    for (std::size_t at = start; at + 480 <= audio.size(); at += 48) {
        if (db(peakOf(audio, at, at + 480) / 0.05) > -1.0) {
            return (at - start) * 1000.0 / kRate;
        }
    }
    return 1e9;
}

void testRelease()
{
    const double fast = recoveryMs(100.0f);
    const double slow = recoveryMs(1000.0f);
    report("100 ms release recovers to -1 dB in about 190 ms", fast > 120.0 && fast < 260.0,
           fmt("%.0f ms", fast));
    report("1000 ms release takes about ten times as long", slow > 1300.0 && slow < 2600.0,
           fmt("%.0f ms", slow));
}

void testCrossoverChangeNoClick()
{
    auto l = makeLimiter(0.0f);
    auto audio = tone(1000.0, 1.0, 0.1);
    const std::size_t mid = audio.size() / 2;
    for (std::size_t at = 0; at < audio.size(); at += kChunk) {
        if (at == mid) {
            l->setCrossoversHz(400.0f, 1500.0f, 4500.0f);
        }
        l->process(audio.data() + at, kChunk, 1);
    }
    double jump = 0.0;
    bool finite = true;
    for (std::size_t i = 1; i < audio.size(); ++i) {
        finite = finite && std::isfinite(audio[i]);
        jump = std::max(jump, std::fabs(static_cast<double>(audio[i]) - audio[i - 1]));
    }
    // A 0.1 sine at 1 kHz moves at most 0.013 per sample.
    report("crossovers change while running without a click", finite && jump < 0.03,
           fmt("largest step %.4f", jump));
}

void testClamps()
{
    ClientMbLimiter l;
    l.setThresholdDb(0, -60.0f);
    l.setThresholdDb(1, 6.0f);
    l.setThresholdDb(2, std::numeric_limits<float>::quiet_NaN());
    report("Threshold clamps to -24..0 dBFS, NaN restores -6",
           l.thresholdDb(0) == -24.0f && l.thresholdDb(1) == 0.0f && l.thresholdDb(2) == -6.0f);
    l.setReleaseMs(0, 1.0f);
    l.setReleaseMs(1, 9000.0f);
    report("Release clamps to 10..1000 ms", l.releaseMs(0) == 10.0f && l.releaseMs(1) == 1000.0f);
    l.setThresholdDb(7, -10.0f);
    l.setReleaseMs(-1, 50.0f);
    report("out-of-range band indices are ignored", l.thresholdDb(7) == 0.0f);

    l.setCrossoverHz(0, 20.0f);
    l.setCrossoverHz(2, 20000.0f);
    report("crossovers clamp to their own ranges",
           l.crossoverHz(0) == 80.0f && l.crossoverHz(2) == 6000.0f);
    l.setCrossoversHz(250.0f, 1000.0f, 3000.0f);
    l.setCrossoverHz(0, 900.0f);
    report("X1 stays 1.5x below X2", l.crossoverHz(0) <= 1000.0f / 1.5f + 0.5f,
           fmt("X1 %.0f", l.crossoverHz(0)));
    l.setCrossoverHz(1, 400.0f);
    report("X2 stays 1.5x above X1 and inside 500..2000",
           l.crossoverHz(1) >= 500.0f && l.crossoverHz(1) >= l.crossoverHz(0) * 1.5f - 0.5f,
           fmt("X2 %.0f", l.crossoverHz(1)));
    l.setCrossoversHz(500.0f, 500.0f, 2000.0f);
    report("all three at once come out ordered",
           l.crossoverHz(0) * 1.5f <= l.crossoverHz(1) + 0.5f
               && l.crossoverHz(1) * 1.5f <= l.crossoverHz(2) + 0.5f,
           fmt("%.0f / %.0f", l.crossoverHz(0), l.crossoverHz(1)));
}

void testReenableClean()
{
    auto l = makeLimiter(-12.0f);
    auto loud = tone(700.0, 0.5, 0.9);
    run(*l, loud);
    l->setEnabled(false);
    l->setEnabled(true);
    std::vector<float> silence(4800, 0.0f);
    run(*l, silence);
    report("re-enabling starts clean: silence in, silence out",
           peakOf(silence, 0, silence.size()) == 0.0);
}

void testStereo()
{
    auto l = makeLimiter(-12.0f);
    auto mono = tone(700.0, 0.2, 0.9);
    std::vector<float> stereo(mono.size() * 2);
    for (std::size_t i = 0; i < mono.size(); ++i) {
        stereo[2 * i] = mono[i];
        stereo[2 * i + 1] = -mono[i];
    }
    for (std::size_t at = 0; at < mono.size(); at += kChunk) {
        l->process(stereo.data() + 2 * at, kChunk, 2);
    }
    bool same = true;
    for (std::size_t i = 0; i < mono.size(); ++i) {
        same = same && stereo[2 * i] == stereo[2 * i + 1];
    }
    report("stereo: the left channel is processed and copied to the right", same);
}

void testJson()
{
    ClientMbLimiter a;
    a.setEnabled(true);
    a.setCrossoversHz(200.0f, 800.0f, 2500.0f);
    a.setThresholdDb(2, -9.0f);
    a.setReleaseMs(3, 300.0f);
    ClientMbLimiter b;
    AetherSDR::applyClientMbLimiterJson(b, AetherSDR::clientMbLimiterToJson(a));
    report("settings JSON round-trips",
           b.isEnabled() && b.crossoverHz(0) == 200.0f && b.crossoverHz(1) == 800.0f
               && b.crossoverHz(2) == 2500.0f && b.thresholdDb(2) == -9.0f && b.releaseMs(3) == 300.0f);

    ClientMbLimiter c;
    QJsonObject bad;
    bad["enabled"] = QStringLiteral("yes");
    bad["x2"] = 99999.0;
    bad["bands"] = QJsonArray{QJsonObject{{"thresholdDb", -99.0}, {"releaseMs", "slow"}}};
    AetherSDR::applyClientMbLimiterJson(c, bad);
    report("mistyped keys keep their value, out-of-range ones clamp",
           !c.isEnabled() && c.crossoverHz(1) == 2000.0f && c.thresholdDb(0) == -24.0f
               && c.releaseMs(0) == 100.0f);
}

}  // namespace

int main()
{
    testDefaults();
    testFlatSum();
    testLatency();
    testBandLimiterNeverExceedsItsThreshold();
    testLookaheadHoldsTheBand();
    testRelease();
    testCrossoverChangeNoClick();
    testClamps();
    testReenableClean();
    testStereo();
    testJson();
    std::printf("%s\n", g_failed == 0 ? "ALL PASSED" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
