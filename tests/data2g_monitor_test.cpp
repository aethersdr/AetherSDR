// Data2gMonitor (RFC #7159, Stage 1): the receive-only adapter over the
// vendored Data2G core, driven the way AetherModem's receive tap will drive
// it — 24 kHz mono float32, arbitrary chunk sizes, an un-normalized level.
//
// The transmitting station here is the vendored core's own Engine, so the
// test proves the whole path the adapter adds: 24 kHz in, decimation to the
// core's 8 kHz, burst search, decode, and the dump handed back.

#include "core/hfdata/Data2gMonitor.h"

#include "arq/engine.hpp"
#include "audio/filters.hpp"
#include "config/config.hpp"
#include "kisslink/kisslink.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using AetherSDR::hfdata::Data2gBurst;
using AetherSDR::hfdata::Data2gMonitor;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    if (ok)
        return;
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
}

// An AX.25 UI frame W1AW>APRS carrying "!beacon", as a KISS client hands it in.
std::vector<std::uint8_t> uiFrame()
{
    std::vector<std::uint8_t> ui;
    const std::pair<std::string, bool> addresses[] = {{"APRS", false}, {"W1AW", true}};
    for (const auto& [call, last] : addresses) {
        for (std::size_t i = 0; i < 6; ++i)
            ui.push_back(static_cast<std::uint8_t>((i < call.size() ? call[i] : ' ') << 1));
        ui.push_back(static_cast<std::uint8_t>(0x60 | (last ? 1 : 0)));
    }
    ui.push_back(0x03);
    ui.push_back(0xF0);
    for (char c : std::string("!beacon"))
        ui.push_back(static_cast<std::uint8_t>(c));
    return ui;
}

// One station sends a broadcast frame into a quiet channel. Returns what a
// receiver hears at 24 kHz: the burst over noise, about 12 dB SNR in 2.5 kHz,
// scaled so the burst peaks near -18 dBFS.
std::vector<float> broadcastOnAir(const std::vector<std::uint8_t>& frame, bool& sent)
{
    namespace cfg = data2g::config;
    constexpr int kBlock = cfg::FS / 10;
    constexpr double kSnrDb = 12.0;
    constexpr double kSignal = 2.2;

    data2g::kisslink::KissLink kiss;
    data2g::arq::EngineConfig engineConfig;
    engineConfig.seed = 41;
    engineConfig.kiss = &kiss;
    data2g::arq::Engine station("W1AW", engineConfig);
    kiss.enqueue(frame);

    std::mt19937 rng(5);
    const double sigma = std::sqrt((cfg::FS / 2.0) / cfg::SNR_REF_BW_HZ / std::pow(10.0, kSnrDb / 10.0));
    std::normal_distribution<double> noise(0.0, sigma);

    std::vector<double> atFs;
    std::vector<double> heardByStation(kBlock);
    bool keyed = false;
    int quietBlocksAfter = 0;
    for (int block = 0; block < 60 * 10 && quietBlocksAfter < 30; ++block) {
        for (double& v : heardByStation)
            v = noise(rng);
        const data2g::arq::Engine::Out out = station.step(heardByStation);
        keyed = keyed || out.ptt;
        if (keyed && !out.ptt && kiss.queue.empty())
            ++quietBlocksAfter;
        for (int i = 0; i < kBlock; ++i) {
            const double tx = i < static_cast<int>(out.audio.size()) ? out.audio[i] : 0.0;
            atFs.push_back(kSignal * tx + noise(rng));
        }
    }
    sent = keyed && kiss.queue.empty();

    data2g::audio::Interpolator to24k(24000);
    const std::vector<double> at24k = to24k(atFs);

    // The burst peaks at kSignal before this; put that at -18 dBFS.
    const double gain = std::pow(10.0, -18.0 / 20.0) / kSignal;
    std::vector<float> pcm(at24k.size());
    std::transform(at24k.begin(), at24k.end(), pcm.begin(),
                   [gain](double v) { return static_cast<float>(v * gain); });
    return pcm;
}

// The adapter fed in uneven chunks, none a multiple of the 3:1 decimation.
std::vector<Data2gBurst> feedInChunks(Data2gMonitor& monitor, const std::vector<float>& pcm)
{
    static constexpr std::size_t kChunks[] = {1, 479, 2401, 7, 1024, 4801, 257};
    std::vector<Data2gBurst> bursts;
    std::size_t offset = 0;
    std::size_t turn = 0;
    while (offset < pcm.size()) {
        const std::size_t n = std::min(kChunks[turn++ % std::size(kChunks)], pcm.size() - offset);
        for (Data2gBurst& burst : monitor.feed(std::span<const float>(pcm.data() + offset, n)))
            bursts.push_back(std::move(burst));
        offset += n;
    }
    return bursts;
}

void rates()
{
    check(Data2gMonitor::isSupportedSampleRate(8000), "8 kHz supported");
    check(Data2gMonitor::isSupportedSampleRate(24000), "24 kHz supported");
    check(Data2gMonitor::isSupportedSampleRate(48000), "48 kHz supported");
    check(!Data2gMonitor::isSupportedSampleRate(44100), "44.1 kHz refused");
    check(!Data2gMonitor::isSupportedSampleRate(0), "0 Hz refused");

    Data2gMonitor bad(44100);
    check(!bad.isValid(), "a monitor at an unsupported rate is invalid");
    const std::vector<float> silence(4800, 0.0f);
    check(bad.feed(silence).empty(), "an invalid monitor decodes nothing");

    check(std::string(Data2gMonitor::upstreamVersion()) == "0.4.3", "the vendored release is 0.4.3");
}

void noiseOnly()
{
    Data2gMonitor monitor;
    check(monitor.isValid() && monitor.sampleRate() == 24000, "the default monitor runs at 24 kHz");
    std::mt19937 rng(9);
    std::normal_distribution<float> noise(0.0f, 0.02f);
    std::vector<float> pcm(24000 * 5);
    for (float& v : pcm)
        v = noise(rng);
    check(feedInChunks(monitor, pcm).empty(), "five seconds of noise yields no burst");
    check(monitor.feed({}).empty(), "an empty chunk yields no burst");
}

void broadcast()
{
    const std::vector<std::uint8_t> frame = uiFrame();
    bool sent = false;
    const std::vector<float> pcm = broadcastOnAir(frame, sent);
    check(sent, "the transmitting station sent its frame");

    Data2gMonitor monitor;
    const std::vector<Data2gBurst> bursts = feedInChunks(monitor, pcm);
    check(!bursts.empty(), "a burst was heard");

    bool groupNamed = false;
    bool frameRead = false;
    bool rendered = false;
    bool timed = false;
    for (const Data2gBurst& burst : bursts) {
        if (burst.head.find("BCAST \"KISS 0\": 1 frame(s)") == std::string::npos)
            continue;
        groupNamed = true;
        timed = burst.startSeconds > 0.0
            && burst.startSeconds < static_cast<double>(pcm.size()) / 24000.0;
        rendered = burst.text.find("W1AW>APRS") != std::string::npos
            && burst.text.find("!beacon") != std::string::npos;
        for (const auto& payload : burst.payloads) {
            if (payload.label == "frame 1 (AX.25 W1AW>APRS, UI)" && payload.bytes == frame
                && std::none_of(payload.unknown.begin(), payload.unknown.end(), [](bool u) { return u; }))
                frameRead = true;
        }
    }
    check(groupNamed, "the broadcast group is named");
    check(frameRead, "the AX.25 frame is read back byte for byte");
    check(rendered, "the rendered text names the stations and shows the payload");
    check(timed, "the burst start falls inside the audio fed");

    if (g_failures) {
        for (const Data2gBurst& burst : bursts)
            std::fputs(burst.text.c_str(), stderr);
    }
}

} // namespace

int main()
{
    Data2gMonitor::setDecodeThreads(2);
    rates();
    noiseOnly();
    broadcast();
    if (g_failures) {
        std::fprintf(stderr, "data2g_monitor_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("ok: data2g_monitor_test");
    return 0;
}
