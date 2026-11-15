// ClientCessb, the AetherTX final peak control (RFC #4836): the SSB envelope
// a radio builds from its output stays at the ceiling, with or without CESSB
// thanks to the ALC backstop; the delay equals the latency it reports; the passband cuts
// what lies outside it and follows the radio TX filter; a passband change
// while running neither gaps the audio nor leaks a block; every range clamps;
// re-enabling starts clean; and the settings JSON round-trips. Socket-free.

#include "core/ClientCessb.h"
#include "core/ClientCessbJson.h"
#include "core/dsp/SsbEnvelopeMeter.h"

#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using AetherSDR::ClientCessb;

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

std::unique_ptr<ClientCessb> makeStage(float clipDb, bool cessb, float outputDb)
{
    auto s = std::make_unique<ClientCessb>();
    s->setClipDb(clipDb);
    s->setOvershootControl(cessb);
    s->setOutputDb(outputDb);
    s->prepare(kRate);
    s->setEnabled(true);
    return s;
}

// A voiced-speech stand-in: a 140 Hz fundamental and its harmonics to `topHz`,
// all in cosine phase so the waveform is peaky, under a 4 Hz syllable swell.
std::vector<float> voice(double seconds, double amplitude, double topHz = 2800.0)
{
    const int n = static_cast<int>(seconds * kRate);
    std::vector<float> out(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double t = i / kRate;
        double v = 0.0;
        for (int h = 1; h * 140.0 < topHz; ++h) {
            v += std::cos(2.0 * kPi * 140.0 * h * t) / std::sqrt(static_cast<double>(h));
        }
        const double swell = 0.55 + 0.45 * std::sin(2.0 * kPi * 4.0 * t);
        out[static_cast<std::size_t>(i)] = static_cast<float>(v * swell);
    }
    float peak = 0.0f;
    for (float v : out) {
        peak = std::max(peak, std::fabs(v));
    }
    for (float& v : out) {
        v = static_cast<float>(v / peak * amplitude);
    }
    return out;
}

std::vector<float> tone(double hz, double seconds, double amplitude)
{
    const int n = static_cast<int>(seconds * kRate);
    std::vector<float> out(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        out[static_cast<std::size_t>(i)] =
            static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * i / kRate));
    }
    return out;
}

// Runs `audio` through the stage in 480-frame mono chunks, in place.
void run(ClientCessb& s, std::vector<float>& audio)
{
    for (std::size_t at = 0; at < audio.size(); at += kChunk) {
        const int frames = static_cast<int>(std::min<std::size_t>(kChunk, audio.size() - at));
        s.process(audio.data() + at, frames, 1);
    }
}

double peakOf(const std::vector<float>& v, std::size_t from = 0)
{
    double p = 0.0;
    for (std::size_t i = from; i < v.size(); ++i) {
        p = std::max(p, std::fabs(static_cast<double>(v[i])));
    }
    return p;
}

double rmsOf(const std::vector<float>& v, std::size_t from, std::size_t to)
{
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        sum += static_cast<double>(v[i]) * v[i];
    }
    return std::sqrt(sum / static_cast<double>(std::max<std::size_t>(1, to - from)));
}

// The envelope a radio with a 100..2900 Hz linear-phase USB filter would put
// on the air from `audio`, past the first `skip` samples.
double radioEnvelopePeakDb(const std::vector<float>& audio, std::size_t skip)
{
    const auto r = AetherSDR::measureSsbEnvelope(audio.data() + skip, audio.size() - skip, kRate,
                                                 {100.0, 2900.0},
                                                 AetherSDR::SsbFilterPhase::Linear);
    return db(r.envelopePeak);
}

void testDefaults()
{
    ClientCessb s;
    report("ships off", !s.isEnabled());
    report("defaults: clip 6 dB, CESSB on, ceiling -1 dB",
           s.clipDb() == 6.0f && s.overshootControl() && s.outputDb() == -1.0f);
    report("default passband 150..2850 Hz", s.lowHz() == 150.0f && s.highHz() == 2850.0f);
    report("no latency before prepare", s.latencyFrames() == 0);
}

void testLatency()
{
    // Low level and no clip gain, so the chain is the three linear-phase
    // bandpasses and the impulse comes out centered on the delay.
    auto s = makeStage(0.0f, true, 0.0f);
    std::vector<float> audio(static_cast<std::size_t>(kRate / 2), 0.0f);
    constexpr std::size_t at = 1000;
    audio[at] = 0.05f;
    run(*s, audio);
    std::size_t argmax = 0;
    for (std::size_t i = 0; i < audio.size(); ++i) {
        if (std::fabs(audio[i]) > std::fabs(audio[argmax])) {
            argmax = i;
        }
    }
    const long measured = static_cast<long>(argmax) - static_cast<long>(at);
    report("the impulse lands at the latency it reports",
           std::labs(measured - s->latencyFrames()) <= 2,
           fmt("measured %.0f, reported %.0f", static_cast<double>(measured),
               static_cast<double>(s->latencyFrames())));
    report("latency is under 40 ms at 48 kHz", s->latencyFrames() < 0.040 * kRate,
           fmt("%.1f ms", s->latencyFrames() * 1000.0 / kRate));
}

void testEnvelopeBound()
{
    const std::size_t skip = static_cast<std::size_t>(0.25 * kRate);
    for (float clip : {6.0f, 12.0f}) {
        for (float ceiling : {-1.0f, -6.0f}) {
            auto s = makeStage(clip, true, ceiling);
            auto audio = voice(2.0, 0.9);
            run(*s, audio);
            const double env = radioEnvelopePeakDb(audio, skip);
            char name[120];
            std::snprintf(name, sizeof name,
                          "clip %.0f, ceiling %.0f: radio envelope stays under the ceiling", clip,
                          ceiling);
            report(name, env <= ceiling + 0.25, fmt("%.2f dBFS vs %.2f", env, ceiling));
        }
    }

    // The range limits, the passband the 8600's 50..4000 Hz SSB filter gives
    // (100..3950), and ESSB filters to the Flex's 10 kHz maximum with a voice
    // that reaches them.
    struct Limit {
        float clip;
        float ceiling;
        double radioLow;
        double radioHigh;
        double tolerance;
        double voiceTopHz;
    };
    for (const Limit& l : {Limit{20.0f, -1.0f, 100.0, 2900.0, 0.25, 2800.0},
                           Limit{6.0f, 0.0f, 100.0, 2900.0, 0.25, 2800.0},
                           Limit{6.0f, -12.0f, 100.0, 2900.0, 0.25, 2800.0},
                           Limit{6.0f, -1.0f, 50.0, 4000.0, 0.25, 2800.0},
                           Limit{20.0f, -1.0f, 50.0, 4000.0, 0.25, 2800.0},
                           Limit{6.0f, -1.0f, 50.0, 6000.0, 0.25, 7000.0},
                           Limit{6.0f, -1.0f, 50.0, 10000.0, 0.25, 11000.0}}) {
        auto s = makeStage(l.clip, true, l.ceiling);
        s->followRadioFilter(static_cast<int>(l.radioLow), static_cast<int>(l.radioHigh));
        auto audio = voice(2.0, 0.9, l.voiceTopHz);
        run(*s, audio);
        const auto r = AetherSDR::measureSsbEnvelope(audio.data() + skip, audio.size() - skip,
                                                     kRate, {l.radioLow, l.radioHigh},
                                                     AetherSDR::SsbFilterPhase::Linear);
        const double env = db(r.envelopePeak);
        char name[120];
        std::snprintf(name, sizeof name, "clip %.0f, ceiling %.0f, radio %.0f..%.0f: within %.2f dB",
                      l.clip, l.ceiling, l.radioLow, l.radioHigh, l.tolerance);
        report(name, env <= l.ceiling + l.tolerance, fmt("%.2f dBFS vs %.2f", env, l.ceiling));
    }

    auto on = makeStage(12.0f, true, -1.0f);
    auto off = makeStage(12.0f, false, -1.0f);
    auto a = voice(2.0, 0.9);
    auto b = a;
    run(*on, a);
    run(*off, b);
    const double envOn = radioEnvelopePeakDb(a, skip);
    const double envOff = radioEnvelopePeakDb(b, skip);
    report("CESSB off, the ALC backstop still holds the ceiling", envOff <= -1.0 + 0.25,
           fmt("on %.2f, off %.2f dBFS", envOn, envOff));
    const double rmsGain = db(rmsOf(a, skip, a.size()) / rmsOf(b, skip, b.size()));
    // The ALC gives up what CESSB would have kept: +0.1 dB on this voice,
    // +0.8 dB on the harness's synthetic speech.
    report("CESSB never costs average power under the same ceiling", rmsGain > 0.0,
           fmt("+%.2f dB with CESSB", rmsGain));

    const float reported = on->takeEnvelopePeakDb();
    report("the reported envelope peak sits at the ceiling",
           reported <= -1.0f + 0.3f && reported >= -1.0f - 1.5f, fmt("%.2f dBFS", reported));
    report("taking the peak resets it", on->takeEnvelopePeakDb() <= -119.0f);

    auto plain = makeStage(0.0f, true, 0.0f);
    auto c = voice(2.0, 0.9);
    auto ref = c;
    run(*plain, c);
    report("more clip raises the average power", rmsOf(a, skip, a.size()) >
                                                     rmsOf(c, skip, c.size()) * 1.25,
           fmt("clip 12: %.2f dB over clip 0",
               db(rmsOf(a, skip, a.size()) / rmsOf(c, skip, c.size()))));
    (void)ref;
}

void testPassband()
{
    const std::size_t skip = static_cast<std::size_t>(0.25 * kRate);
    auto s = makeStage(0.0f, true, 0.0f);
    auto in1k = tone(1000.0, 1.0, 0.05);
    auto in35 = tone(3500.0, 1.0, 0.05);
    auto in50 = tone(50.0, 1.0, 0.05);
    run(*s, in1k);
    s->reset();
    run(*s, in35);
    s->reset();
    run(*s, in50);
    const double pass = peakOf(in1k, skip);
    report("1 kHz passes at unity", std::fabs(db(pass / 0.05)) < 0.5, fmt("%.2f dB", db(pass / 0.05)));
    report("3.5 kHz, above the passband, is cut by 30 dB or more",
           db(peakOf(in35, skip) / 0.05) < -30.0, fmt("%.1f dB", db(peakOf(in35, skip) / 0.05)));
    report("50 Hz, below the passband, is cut by 30 dB or more",
           db(peakOf(in50, skip) / 0.05) < -30.0, fmt("%.1f dB", db(peakOf(in50, skip) / 0.05)));

    s->followRadioFilter(50, 4000);
    report("follows a 50..4000 Hz radio filter at 100..3950",
           s->lowHz() == 100.0f && s->highHz() == 3950.0f,
           fmt("%.0f..%.0f", s->lowHz(), s->highHz()));
    auto wide = tone(3500.0, 1.5, 0.05);
    run(*s, wide);
    report("after it, 3.5 kHz passes", std::fabs(db(peakOf(wide, wide.size() / 2) / 0.05)) < 1.0,
           fmt("%.2f dB", db(peakOf(wide, wide.size() / 2) / 0.05)));
    s->followRadioFilter(3000, 2000);
    report("an inverted radio filter is ignored", s->lowHz() == 100.0f && s->highHz() == 3950.0f);
    s->followRadioFilter(50, 10000);
    report("follows a 10 kHz ESSB filter at 100..9950",
           s->lowHz() == 100.0f && s->highHz() == 9950.0f,
           fmt("%.0f..%.0f", s->lowHz(), s->highHz()));
}

void testRuntimeSwap()
{
    const int before = ClientCessb::liveBlockCount();
    {
        auto s = makeStage(0.0f, true, 0.0f);
        report("one live block once prepared", ClientCessb::liveBlockCount() == before + 1);
        auto audio = tone(1000.0, 3.0, 0.05);
        const std::size_t half = audio.size() / 3;
        // Change the passband three times mid-stream, as the operator dragging
        // the radio filter would, the owner side between chunks.
        int changes = 0;
        for (std::size_t at = 0; at < audio.size(); at += kChunk) {
            if (at == half || at == half + 4800 || at == half + 9600) {
                s->setPassband(200.0f + 50.0f * changes, 2600.0f + 100.0f * changes);
                ++changes;
            }
            s->process(audio.data() + at, kChunk, 1);
        }
        // Every 10 ms window from settled onward keeps the tone's level.
        const std::size_t settle = static_cast<std::size_t>(0.25 * kRate);
        double worst = 0.0;
        for (std::size_t at = settle; at + 480 <= audio.size(); at += 480) {
            const double level = db(rmsOf(audio, at, at + 480) / (0.05 / std::sqrt(2.0)));
            worst = std::max(worst, std::fabs(level));
        }
        report("passband changes while running: no gap, no jump", worst < 0.5,
               fmt("worst 10 ms window %.2f dB off", worst));
        report("the last passband is the one in force",
               s->lowHz() == 300.0f && s->highHz() == 2800.0f,
               fmt("%.0f..%.0f", s->lowHz(), s->highHz()));
        s->collectRetiredBlocks();
        report("superseded blocks are freed on the owner side",
               ClientCessb::liveBlockCount() == before + 1,
               fmt("%.0f live", static_cast<double>(ClientCessb::liveBlockCount() - before)));

        // A change before prepare is just the passband prepare builds at.
        ClientCessb cold;
        cold.setPassband(400.0f, 2400.0f);
        report("a change before prepare builds nothing",
               ClientCessb::liveBlockCount() == before + 1);
    }
    report("destruction frees every block", ClientCessb::liveBlockCount() == before);
}

void testClamps()
{
    ClientCessb s;
    s.setClipDb(50.0f);
    const bool clipHi = s.clipDb() == ClientCessb::kMaxClipDb;
    s.setClipDb(-5.0f);
    const bool clipLo = s.clipDb() == 0.0f;
    s.setClipDb(std::numeric_limits<float>::quiet_NaN());
    const bool clipNan = s.clipDb() == ClientCessb::kDefaultClipDb;
    report("Clip clamps to 0..20 dB and NaN restores the default", clipHi && clipLo && clipNan);

    s.setOutputDb(-40.0f);
    const bool outLo = s.outputDb() == ClientCessb::kMinOutputDb;
    s.setOutputDb(3.0f);
    const bool outHi = s.outputDb() == ClientCessb::kMaxOutputDb;
    report("Ceiling clamps to -12..0 dB", outLo && outHi);

    s.setPassband(0.0f, 100.0f);
    report("passband floor 100 Hz, at least 500 Hz wide",
           s.lowHz() == 100.0f && s.highHz() == 600.0f, fmt("%.0f..%.0f", s.lowHz(), s.highHz()));
    s.setPassband(9800.0f, 15000.0f);
    report("passband ceiling 9950 Hz, at least 500 Hz wide",
           s.lowHz() == 9450.0f && s.highHz() == 9950.0f, fmt("%.0f..%.0f", s.lowHz(), s.highHz()));
}

void testReenableClean()
{
    auto s = makeStage(12.0f, true, -1.0f);
    auto loud = voice(0.5, 0.9);
    run(*s, loud);
    s->setEnabled(false);
    s->setEnabled(true);
    std::vector<float> silence(static_cast<std::size_t>(0.2 * kRate), 0.0f);
    run(*s, silence);
    report("re-enabling starts clean: silence in, silence out", peakOf(silence) < 1e-6,
           fmt("%.2e", peakOf(silence)));
}

void testStereo()
{
    auto s = makeStage(6.0f, true, -1.0f);
    auto mono = voice(0.5, 0.9);
    std::vector<float> stereo(mono.size() * 2);
    for (std::size_t i = 0; i < mono.size(); ++i) {
        stereo[2 * i] = mono[i];
        stereo[2 * i + 1] = -mono[i];
    }
    for (std::size_t at = 0; at < mono.size(); at += kChunk) {
        s->process(stereo.data() + 2 * at, kChunk, 2);
    }
    bool same = true;
    for (std::size_t i = 0; i < mono.size(); ++i) {
        same = same && stereo[2 * i] == stereo[2 * i + 1];
    }
    report("stereo: the left channel is processed and copied to the right", same);
}

void testJson()
{
    ClientCessb a;
    a.setEnabled(true);
    a.setClipDb(9.5f);
    a.setOvershootControl(false);
    a.setOutputDb(-3.0f);
    ClientCessb b;
    AetherSDR::applyClientCessbJson(b, AetherSDR::clientCessbToJson(a));
    report("settings JSON round-trips",
           b.isEnabled() && b.clipDb() == 9.5f && !b.overshootControl() && b.outputDb() == -3.0f);

    ClientCessb c;
    QJsonObject bad;
    bad["enabled"] = QStringLiteral("yes");
    bad["clipDb"] = 99.0;
    bad["outputDb"] = QStringLiteral("loud");
    AetherSDR::applyClientCessbJson(c, bad);
    report("mistyped keys keep their value, out-of-range ones clamp",
           !c.isEnabled() && c.clipDb() == ClientCessb::kMaxClipDb &&
               c.outputDb() == ClientCessb::kDefaultOutputDb);
    report("the passband is not a setting",
           !AetherSDR::clientCessbToJson(a).contains(QStringLiteral("lowHz")) &&
               !AetherSDR::clientCessbToJson(a).contains(QStringLiteral("highHz")));
}

}  // namespace

int main()
{
    testDefaults();
    testLatency();
    testEnvelopeBound();
    testPassband();
    testRuntimeSwap();
    testClamps();
    testReenableClean();
    testStereo();
    testJson();
    std::printf("%s\n", g_failed == 0 ? "ALL PASSED" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
