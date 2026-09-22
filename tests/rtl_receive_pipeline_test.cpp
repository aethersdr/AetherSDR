#include "core/backends/rtl/RtlReceivePipeline.h"
#include "CallbackAllocationProbe.h"
#include <aether_wdsp.h>
#include <QCoreApplication>
#include <QThreadPool>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <limits>
#include <numbers>
#include <thread>
#include <vector>

using Pipeline = AetherSDR::rtl::RtlReceivePipeline;
using T = AetherSDR::rtl::RtlCaptureTransaction;
using namespace std::chrono_literals;
namespace AetherSDR::rtl {
struct RtlReceivePipelineTestAccess {
    static void exhaustEpoch(RtlReceivePipeline& pipeline)
    { pipeline.m_nextEpoch = std::numeric_limits<std::uint64_t>::max(); }
    static std::uint64_t requested(RtlReceivePipeline& pipeline)
    { return pipeline.m_registry.service().requested; }
    static bool rejectMalformedMixer(RtlReceivePipeline& pipeline)
    {
        pipeline.m_legacy = false; pipeline.m_token = {1, 1}; pipeline.m_captureEpoch = 1;
        pipeline.m_capture = {1, 1, 100000000, 2400000, 1080000, 1080000};
        const std::array<RtlAudioMixer::Input, 1> inputs{{{0, 1, 1}}};
        std::array<float, 128> audio; audio.fill(0.75f);
        if (!pipeline.m_mixer.configure(1, 1, inputs, 0)
            || !pipeline.m_mixer.push(0, 1, 1, 0, audio, audio)) { return false; }
        RtlReceiverRegistry::ReceiverSpec malformed;
        malformed.handle.slot = 0; malformed.handle.instance = 0;
        const std::array<RtlReceiverRegistry::ReceiverView, 1> views{{{&malformed, nullptr}}};
        inCallback = true;
        pipeline.process(RtlReceiverRegistry::SampleBlock{}, views);
        pipeline.m_mixer.drain(4096, pipeline);
        inCallback = false;
        RtlReceivePipeline::Packet packet;
        return !pipeline.takePacket(packet) && pipeline.needsRepair()
            && pipeline.diagnostics().mixerConfigurationFailures == 1;
    }
};
}
static int failures = 0;
static void check(bool value, const char* message)
{ if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); } }
static bool ready(Pipeline& pipeline)
{
    const auto end = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < end) {
        const auto state = pipeline.service();
        if (state == Pipeline::Preparation::Ready) { return true; }
        if (state == Pipeline::Preparation::Failed) { return false; }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}
static double magnitude(const std::vector<float>& input, double tone)
{
    std::complex<double> sum{};
    for (std::size_t n = 4096; n < input.size(); ++n) {
        const double phase = -2 * std::numbers::pi * tone * n / 48000;
        sum += double(input[n]) * std::complex<double>(std::cos(phase), std::sin(phase));
    }
    return std::abs(sum) / std::max<std::size_t>(1, input.size());
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    {
        auto candidate = std::make_unique<Pipeline>();
        T::State accepted;
        accepted.token = {7, 1};
        accepted.capture = {7, 1, 100000000, 2400000, 1080000, 1080000};
        accepted.receivers = {{{2, 100000000, -8000, 8000, 0, 3000, 3000}, T::Mode::Fm}};
        check(candidate->prepare(accepted, true) && ready(*candidate), "valid pending bank prepared before refusal tests");
        using Access = AetherSDR::rtl::RtlReceivePipelineTestAccess;
        const auto revision = Access::requested(*candidate);
        for (int bad = 0; bad < 5; ++bad) {
            auto invalid = accepted; invalid.token.revision = 2;
            if (bad == 0) { invalid.receivers[0].audioGain = -1; }
            if (bad == 1) { invalid.receivers[0].audioPan = 101; }
            if (bad == 2) { invalid.receivers[0].passband.stableId = 8; }
            if (bad == 3) { invalid.receivers.push_back(invalid.receivers[0]); }
            if (bad == 4) { invalid.receivers[0].mode = T::Mode::Wfm; invalid.receivers[0].audioGain = 101; }
            check(!candidate->prepare(invalid), "invalid complete input refused before submission");
            check(Access::requested(*candidate) == revision, "refused input cannot replace the pending registry request");
        }
        check(ready(*candidate) && candidate->adopt(), "original pending bank still adopts after malformed attempts");
        Access::exhaustEpoch(*candidate);
        check(!candidate->prepare(accepted, true) && Access::requested(*candidate) == revision,
            "capture epoch exhaustion refuses before registry submission");
        candidate->stop();
    }
    check(QThreadPool::globalInstance()->waitForDone(15000), "refusal test receivers retired");
    {
        auto failed = std::make_unique<Pipeline>();
        check(AetherSDR::rtl::RtlReceivePipelineTestAccess::rejectMalformedMixer(*failed),
            "rejected mixer configuration discards old queued audio, counts failure and requests repair");
        check(failed->diagnostics().observed && callbackAllocations == 0,
            "mixer failure path is observable and allocation-free");
    }
    auto pipeline = std::make_unique<Pipeline>(4); // measurement workload, not advertised capacity
    T::State state;
    state.token = {42, 1}; state.hardware.centerHz = 100000000;
    state.capture = {42, 1, 100000000, 2400000, 1080000, 1080000};
    constexpr std::array<double, 4> offsets{-1062000, -400000, 400000, 1062000};
    constexpr std::array<double, 4> tones{701, 1093, 1601, 2203};
    for (int id = 0; id < 4; ++id) {
        state.receivers.push_back({{id, 100000000 + offsets[id], -15000, 15000, 0, 3000, 3000}, T::Mode::Fm});
    }
    state.receivers[0].audioGain = 0;
    state.receivers[0].audioMute = true;
    check(pipeline->prepare(state, true) && ready(*pipeline) && pipeline->adopt(), "four real independent WDSP receivers prepare and adopt");
    constexpr std::size_t total = 1440000;
    std::vector<std::complex<float>> iq(total);
    for (std::size_t n = 0; n < total; ++n) {
        const double time = n / 2400000.0;
        for (int id = 0; id < 4; ++id) {
            const double phase = 2 * std::numbers::pi * offsets[id] * time
                + 3500 / tones[id] * std::sin(2 * std::numbers::pi * tones[id] * time);
            iq[n] += std::complex<float>(0.15 * std::cos(phase), 0.15 * std::sin(phase));
        }
    }
    std::array<std::vector<float>, 4> audio;
    for (auto& samples : audio) { samples.reserve(48000); }
    std::vector<float> speaker; speaker.reserve(48000);
    std::array<std::uint64_t, 4> next{};
    std::uint64_t speakerNext = 0;
    Pipeline::Packet packet;
    (void)wdspPortThreadAllocationSequence(); // materialize TLS before measurement
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t first = 0; first < total;) {
        const std::size_t size = std::min<std::size_t>(8192, total - first);
        inCallback = true;
        const bool accepted = pipeline->process(first, std::span(iq).subspan(first, size));
        inCallback = false;
        check(accepted, "capture block accepted");
        first += size;
        while (pipeline->takePacket(packet)) {
            check(packet.token == state.token && packet.captureEpoch != 0, "production queue retains acquisition identity");
            if (packet.slot < 0) {
                check(packet.firstSample == speakerNext, "one monotonic speaker stream without duplicate feed");
                speakerNext += packet.frames;
                for (std::size_t i = 0; i < packet.frames; ++i) { speaker.push_back(packet.samples[2 * i]); }
            } else {
                const int id = packet.slot;
                check(id < 4 && packet.firstSample == next[id] && packet.instance != 0, "slice tap retains independent continuous capture positions");
                next[id] += packet.frames;
                for (std::size_t i = 0; i < packet.frames; ++i) { audio[id].push_back(packet.samples[2 * i]); }
            }
        }
        std::this_thread::sleep_until(start + std::chrono::microseconds(first * 1000000 / 2400000));
    }
    check(!pipeline->needsRepair() && pipeline->droppedPackets() == 0, "paced whole pipeline has no withdrawal or queue drop");
    for (int id = 0; id < 4; ++id) {
        const double selected = magnitude(audio[id], tones[id]);
        check(audio[id].size() > 20000 && selected > 1e-5, "each real WDSP channel demodulates its own carrier including muted tap");
        for (int other = 0; other < 4; ++other) {
            if (id == other) { continue; }
            const double rejection = 20 * std::log10(selected / std::max(1e-12, magnitude(audio[id], tones[other])));
            std::printf("WDSP carrier %d versus %d: %.1f dB\n", id, other, rejection);
            check(rejection > 20, "real WDSP output separates all four distinct FM carriers");
        }
    }
    check(magnitude(speaker, tones[0]) < magnitude(audio[0], tones[0]) * 0.02,
        "monitor mute does not leak selected signal into speaker or destroy its independent tap");
    check(speakerNext > 20000 && speakerNext <= total / 50, "48 kHz speaker duration follows capture sample count");
    check(callbackAllocations == 0, "integrated acquisition path performs no ordinary C++ allocation");
    pipeline->stop(); pipeline.reset();
    check(QThreadPool::globalInstance()->waitForDone(15000), "receiver destruction drains off acquisition");
    std::fprintf(stderr, "rtl_receive_pipeline_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
