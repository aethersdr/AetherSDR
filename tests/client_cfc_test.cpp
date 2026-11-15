// ClientCfc, the AetherTX stage around WDSP's CFC (RFC #4836): unity on a flat
// profile, a delay equal to the latency it reports at every FFT size, per-band
// gain, pre-comp and post-EQ, the range limits, an FFT-size change with no gap
// and no leaked block, a clean restart after re-enabling, and the live
// compression display. Socket-free; no radio, no audio device.

#include "core/ClientCfc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using AetherSDR::ClientCfc;

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

std::vector<ClientCfc::Band> flat()
{
    return {{200.0f, 0.0f, 0.0f}, {1000.0f, 0.0f, 0.0f}, {4000.0f, 0.0f, 0.0f}};
}

std::unique_ptr<ClientCfc> makeCfc(int fftSize, const std::vector<ClientCfc::Band>& bands)
{
    auto cfc = std::make_unique<ClientCfc>();
    cfc->setFftSize(fftSize);
    cfc->setBands(bands);
    cfc->prepare(kRate);
    cfc->setEnabled(true);
    return cfc;
}

// Runs `seconds` of a sine through in 480-frame stereo blocks and returns the
// output's peak over the last quarter (steady state) divided by the input's.
double steadyGainDb(ClientCfc& cfc, double hz, double amplitude, double seconds = 1.5)
{
    const int total = static_cast<int>(seconds * kRate);
    std::vector<float> block(480 * 2);
    double peak = 0.0;
    for (int at = 0; at < total; at += 480) {
        for (int i = 0; i < 480; ++i) {
            const float v = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * (at + i) / kRate));
            block[static_cast<std::size_t>(i) * 2] = v;
            block[static_cast<std::size_t>(i) * 2 + 1] = v;
        }
        cfc.process(block.data(), 480, 2);
        if (at >= total * 3 / 4) {
            for (int i = 0; i < 480; ++i) {
                peak = std::max(peak, std::fabs(static_cast<double>(block[static_cast<std::size_t>(i) * 2])));
            }
        }
    }
    return 20.0 * std::log10(std::max(peak, 1e-12) / amplitude);
}

void testDefaults()
{
    ClientCfc cfc;
    report("ships off", !cfc.isEnabled());
    report("first-enable profile is create_txa's", cfc.bands() == ClientCfc::defaultBands());
    report("post-EQ off, pre-comp and post gain 0",
           !cfc.postEqEnabled() && cfc.precompDb() == 0.0f && cfc.postGainDb() == 0.0f);
    report("FFT size defaults to 1024", cfc.fftSize() == 1024);
}

void testUnityAndDelay()
{
    for (int fft : {1024, 2048, 4096, 8192, 16384}) {
        auto cfc = makeCfc(fft, flat());
        // Mono, so the delay is easy to read: one impulse, then silence.
        const int total = fft + 4096;
        std::vector<float> x(static_cast<std::size_t>(total), 0.0f);
        x[100] = 0.01f;
        for (int at = 0; at < total; at += 480) {
            cfc->process(x.data() + at, std::min(480, total - at), 1);
        }
        int peakAt = -1;
        float peak = 0.0f;
        for (int i = 0; i < total; ++i) {
            if (std::fabs(x[static_cast<std::size_t>(i)]) > peak) {
                peak = std::fabs(x[static_cast<std::size_t>(i)]);
                peakAt = i;
            }
        }
        const std::string name = "FFT " + std::to_string(fft) + ": delay equals latencyFrames()";
        report(name.c_str(), peakAt - 100 == cfc->latencyFrames() && cfc->latencyFrames() == fft,
               fmt("delay %.0f, reported %.0f", peakAt - 100, cfc->latencyFrames()));
    }
    auto cfc = makeCfc(1024, flat());
    const double g = steadyGainDb(*cfc, 1000.0, 0.1);
    report("flat profile passes a tone at unity", std::fabs(g) < 0.1, fmt("%+.3f dB", g));
}

void testPerBandGain()
{
    // 20 dB from 800 to 1250 Hz only. A quiet tone is raised there and nowhere else; a
    // loud one at 1 kHz is held at the block's ceiling instead.
    const std::vector<ClientCfc::Band> peak{{200.0f, 0.0f, 0.0f}, {600.0f, 0.0f, 0.0f},
                                            {800.0f, 20.0f, 0.0f}, {1250.0f, 20.0f, 0.0f},
                                            {1500.0f, 0.0f, 0.0f}, {4000.0f, 0.0f, 0.0f}};
    auto atBand = makeCfc(2048, peak);
    const double quietAt1k = steadyGainDb(*atBand, 1000.0, 0.001);
    auto offBand = makeCfc(2048, peak);
    const double quietAt3k = steadyGainDb(*offBand, 3000.0, 0.001);
    auto loud = makeCfc(2048, peak);
    const double loudAt1k = steadyGainDb(*loud, 1000.0, 0.5);
    report("a band's compression raises a quiet tone by its level", std::fabs(quietAt1k - 20.0) < 1.0,
           fmt("%+.2f dB at 1 kHz", quietAt1k));
    report("and leaves other frequencies alone", std::fabs(quietAt3k) < 0.5,
           fmt("%+.2f dB at 3 kHz", quietAt3k));
    report("a loud tone in that band is held, not raised 20 dB", loudAt1k < 10.0,
           fmt("%+.2f dB at 1 kHz, 0.5 in", loudAt1k));

    auto pre = makeCfc(2048, flat());
    pre->setPrecompDb(10.0f);
    const double preLow = steadyGainDb(*pre, 500.0, 0.001);
    auto pre2 = makeCfc(2048, flat());
    pre2->setPrecompDb(10.0f);
    const double preHigh = steadyGainDb(*pre2, 2500.0, 0.001);
    report("pre-comp raises every frequency alike",
           std::fabs(preLow - 10.0) < 1.0 && std::fabs(preHigh - 10.0) < 1.0,
           fmt("%+.2f dB at 500 Hz, %+.2f dB at 2.5 kHz", preLow, preHigh));
}

void testPostEq()
{
    const std::vector<ClientCfc::Band> plus6{{200.0f, 0.0f, 6.0f}, {1000.0f, 0.0f, 6.0f},
                                             {4000.0f, 0.0f, 6.0f}};
    auto off = makeCfc(1024, plus6);
    const double gOff = steadyGainDb(*off, 1000.0, 0.01);
    auto on = makeCfc(1024, plus6);
    on->setPostEqEnabled(true);
    const double gOn = steadyGainDb(*on, 1000.0, 0.01);
    auto gained = makeCfc(1024, plus6);
    gained->setPostEqEnabled(true);
    gained->setPostGainDb(3.0f);
    const double gGain = steadyGainDb(*gained, 1000.0, 0.01);
    report("post-EQ off ignores its curve", std::fabs(gOff) < 0.1, fmt("%+.2f dB", gOff));
    report("post-EQ on applies it", std::fabs(gOn - 6.0) < 0.2, fmt("%+.2f dB", gOn));
    report("post gain adds to it", std::fabs(gGain - 9.0) < 0.2, fmt("%+.2f dB", gGain));
}

void testRangeLimits()
{
    ClientCfc cfc;
    cfc.setFftSize(100);
    const int low = cfc.fftSize();
    cfc.setFftSize(99999);
    const int high = cfc.fftSize();
    cfc.setFftSize(1400);
    const int near1024 = cfc.fftSize();
    cfc.setFftSize(3000);
    const int near4096 = cfc.fftSize();
    report("FFT size snaps to a power of two in 1024..16384",
           low == 1024 && high == 16384 && near1024 == 1024 && near4096 == 4096);

    cfc.setPrecompDb(50.0f);
    const float preHigh = cfc.precompDb();
    cfc.setPrecompDb(-5.0f);
    const float preLow = cfc.precompDb();
    cfc.setPrecompDb(std::numeric_limits<float>::quiet_NaN());
    report("pre-comp clamps to 0..20 dB and refuses NaN",
           preHigh == 20.0f && preLow == 0.0f && cfc.precompDb() == 0.0f);

    cfc.setPostGainDb(30.0f);
    const float gainHigh = cfc.postGainDb();
    cfc.setPostGainDb(-30.0f);
    report("post gain clamps to -20..+20 dB", gainHigh == 20.0f && cfc.postGainDb() == -20.0f);

    cfc.setBands({{20000.0f, 50.0f, 50.0f}, {1.0f, -3.0f, -50.0f}});
    const auto clamped = cfc.bands();
    report("bands clamp and sort",
           clamped.size() == 2 && clamped[0] == ClientCfc::Band{10.0f, 0.0f, -20.0f}
               && clamped[1] == ClientCfc::Band{9990.0f, 20.0f, 20.0f});

    cfc.setBands({{500.0f, 3.0f, 0.0f}});
    report("fewer than two bands falls back to the default profile",
           cfc.bands() == ClientCfc::defaultBands());

    std::vector<ClientCfc::Band> twelve;
    for (int i = 0; i < 12; ++i) {
        twelve.push_back({100.0f * (i + 1), 1.0f, 0.0f});
    }
    cfc.setBands(twelve);
    report("more than ten bands keeps the first ten", cfc.bands().size() == 10);
}

void testFftSizeChangeWhileRunning()
{
    const int before = ClientCfc::liveBlockCount();
    {
        auto cfc = makeCfc(1024, flat());
        std::vector<float> block(480 * 2);
        bool finite = true;
        double quietestAfterFill = 1.0;
        long n = 0;
        for (int b = 0; b < 400; ++b) {  // 4 s
            if (b == 100) {
                cfc->setFftSize(4096);
            }
            for (int i = 0; i < 480; ++i, ++n) {
                const float v = static_cast<float>(0.1 * std::sin(2.0 * kPi * 1000.0 * n / kRate));
                block[static_cast<std::size_t>(i) * 2] = v;
                block[static_cast<std::size_t>(i) * 2 + 1] = v;
            }
            cfc->process(block.data(), 480, 2);
            double blockPeak = 0.0;
            for (float v : block) {
                finite = finite && std::isfinite(v);
                blockPeak = std::max(blockPeak, std::fabs(static_cast<double>(v)));
            }
            if (b > 10) {
                quietestAfterFill = std::min(quietestAfterFill, blockPeak);
            }
        }
        cfc->collectRetiredBlocks();
        report("an FFT change mid-stream stays finite", finite);
        report("and has no gap: every 10 ms block keeps the tone", quietestAfterFill > 0.09,
               fmt("quietest block peak %.4f", quietestAfterFill));
        report("and the reported latency follows it", cfc->latencyFrames() == 4096);
        report("and only the live block remains", ClientCfc::liveBlockCount() == before + 1,
               fmt("%.0f live", ClientCfc::liveBlockCount()));
    }
    report("destroying the stage frees every block", ClientCfc::liveBlockCount() == before);
}

void testReenableStartsClean()
{
    auto cfc = makeCfc(1024, flat());
    steadyGainDb(*cfc, 1000.0, 0.5, 0.5);
    cfc->setEnabled(false);
    cfc->setEnabled(true);
    std::vector<float> silence(480, 0.0f);
    cfc->process(silence.data(), 480, 1);
    const bool clean = std::all_of(silence.begin(), silence.end(), [](float v) { return v == 0.0f; });
    report("re-enabling does not replay audio from the last run", clean);
}

void testCompressionDisplay()
{
    const std::vector<ClientCfc::Band> peak{{200.0f, 0.0f, 0.0f}, {600.0f, 0.0f, 0.0f},
                                            {800.0f, 20.0f, 0.0f}, {1250.0f, 20.0f, 0.0f},
                                            {1500.0f, 0.0f, 0.0f}, {4000.0f, 0.0f, 0.0f}};
    auto cfc = makeCfc(2048, peak);
    std::array<float, ClientCfc::kDisplayPoints> display{};
    report("nothing is displayed before it is asked for", !cfc->compressionDisplay(display));
    cfc->setCompressionDisplayEnabled(true);
    steadyGainDb(*cfc, 1000.0, 0.5);
    const bool got = cfc->compressionDisplay(display);
    // The most compression shown between two frequencies.
    const auto most = [&](double fromHz, double toHz) {
        float m = 0.0f;
        for (int i = 0; i < ClientCfc::kDisplayPoints; ++i) {
            const double hz = 24000.0 * i / (ClientCfc::kDisplayPoints - 1);
            if (hz >= fromHz && hz <= toHz) {
                m = std::max(m, display[static_cast<std::size_t>(i)]);
            }
        }
        return m;
    };
    report("the display shows compression at the loud band and none away from it",
           got && most(900.0, 1100.0) > 6.0f && most(2500.0, 3500.0) < 0.5f,
           fmt("%.1f dB near 1 kHz, %.1f dB near 3 kHz", most(900.0, 1100.0), most(2500.0, 3500.0)));
}

}  // namespace

int main()
{
    testDefaults();
    testUnityAndDelay();
    testPerBandGain();
    testPostEq();
    testRangeLimits();
    testFftSizeChangeWhileRunning();
    testReenableStartsClean();
    testCompressionDisplay();
    std::printf("%s\n", g_failed ? "FAILED" : "PASSED");
    return g_failed ? 1 : 0;
}
