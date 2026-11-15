// Data2gMonitor (RFC #7159, Stage 1): the receive-only adapter over the
// vendored Data2G core, driven the way AetherModem's receive tap will drive
// it — 24 kHz mono float32, arbitrary chunk sizes, an un-normalized level.
//
// The transmitting station here is the vendored core's own Engine, so the
// test proves the whole path the adapter adds: 24 kHz in, decimation to the
// core's 8 kHz, burst search, decode, and the dump handed back.

#include "core/hfdata/Data2gMonitor.h"

#include "data2g_test_signal.h"


#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using AetherSDR::hfdata::Data2gBurst;
using AetherSDR::hfdata::Data2gMonitor;

using AetherSDR::test::broadcastOnAir;
using AetherSDR::test::uiFrame;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    if (ok)
        return;
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
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

    check(std::string(Data2gMonitor::upstreamVersion()) == "0.5.0", "the vendored version is 0.5.0");
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

// A source change mid-burst: the first half is forgotten, and the burst that
// follows decodes exactly once, timed from the reset.
void resetMidBurst()
{
    const std::vector<std::uint8_t> frame = uiFrame();
    bool sent = false;
    const std::vector<float> pcm = broadcastOnAir(frame, sent);
    check(sent, "reset: the transmitting station sent its frame");

    Data2gMonitor monitor;
    monitor.setStamp([](double) { return std::string("STAMP"); });

    // Where the burst sits: the loudest sample is well inside it.
    std::size_t loudest = 0;
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        if (std::fabs(pcm[i]) > std::fabs(pcm[loudest]))
            loudest = i;
    }
    const std::vector<float> head(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(loudest));
    check(feedInChunks(monitor, head).empty(), "reset: half a burst decodes nothing");
    monitor.reset();

    // The rest of the old stream must not complete the burst the reset forgot:
    // a reset that kept the first half would decode it here.
    const std::vector<float> tail(pcm.begin() + static_cast<std::ptrdiff_t>(loudest), pcm.end());
    check(feedInChunks(monitor, tail).empty(), "reset: the old stream's remainder cannot finish its burst");
    monitor.reset();

    int heard = 0;
    bool stamped = false;
    bool timedFromReset = false;
    for (const Data2gBurst& burst : feedInChunks(monitor, pcm)) {
        if (burst.head.find("BCAST \"KISS 0\": 1 frame(s)") == std::string::npos)
            continue;
        ++heard;
        stamped = burst.head.rfind("STAMP", 0) == 0;
        timedFromReset = burst.startSeconds < static_cast<double>(pcm.size()) / 24000.0;
    }
    check(heard == 1, "reset: the whole burst after it decodes exactly once");
    check(stamped, "reset: the caller's stamp opens the burst's first line");
    check(timedFromReset, "reset: burst time restarts at the reset");
    check(monitor.faultCount() == 0, "reset: no fault was counted");
}

} // namespace

int main()
{
    Data2gMonitor::setDecodeThreads(2);
    rates();
    noiseOnly();
    broadcast();
    resetMidBurst();
    if (g_failures) {
        std::fprintf(stderr, "data2g_monitor_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("ok: data2g_monitor_test");
    return 0;
}
