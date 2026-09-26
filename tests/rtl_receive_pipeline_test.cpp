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
#ifdef AETHER_BACKEND_RTL
#include "core/backends/rtl/RtlSdrDdc.h"
#endif

using Pipeline = AetherSDR::rtl::RtlReceivePipeline;
using T = AetherSDR::rtl::RtlCaptureTransaction;
using namespace std::chrono_literals;
namespace AetherSDR::rtl {
struct RtlReceivePipelineTestAccess {
    static void exhaustEpoch(RtlReceivePipeline& pipeline)
    { pipeline.m_nextEpoch = std::numeric_limits<std::uint64_t>::max(); }
    static std::uint64_t requested(RtlReceivePipeline& pipeline)
    { return pipeline.m_registry.service().requested; }
    static std::uint64_t receiverEpoch(RtlReceivePipeline& pipeline, int slot)
    { return pipeline.m_specs[slot].epoch; }
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
static void measureSingleFmReceiver(T::Mode mode)
{
    auto pipeline = std::make_unique<Pipeline>();
    T::State state;
    state.token = {99, 1};
    state.capture = {99, 1, 100000000, 2400000, 1080000, 1080000};
    state.receivers = {{{0, 100000000, -8000, 8000, 0, 3000, 3000}, mode}};
    state.receivingIds = {0};
    check(pipeline->prepare(state, true) && ready(*pipeline) && pipeline->adopt(),
        "single FM audio reproduction prepares the production graph");
    std::array<std::complex<float>, 8192> iq;
    std::array<double, 2> peak{}, energy{};
    std::array<std::size_t, 2> count{}, outside{}, clipped{};
    constexpr std::size_t total = 2400000;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t first = 0; first < total;) {
        const std::size_t size = std::min(iq.size(), total - first);
        for (std::size_t i = 0; i < size; ++i) {
            const double time = (first + i) / 2400000.0;
            const double phase = 2.5 * std::sin(2 * std::numbers::pi * 1000 * time);
            iq[i] = std::complex<float>(0.3 * std::cos(phase), 0.3 * std::sin(phase));
        }
        check(pipeline->process(first, std::span(iq).first(size)), "single FM IQ accepted");
        first += size;
        Pipeline::Packet packet;
        while (pipeline->takePacket(packet)) {
            const int stream = packet.slot < 0 ? 1 : 0;
            for (std::size_t i = 0; i < packet.frames; ++i) {
                if (packet.firstSample + i < 24000) { continue; }
                const double value = packet.samples[2 * i];
                peak[stream] = std::max(peak[stream], std::abs(value));
                energy[stream] += value * value;
                ++count[stream];
                outside[stream] += std::abs(value) > 1.0 ? 1 : 0;
                clipped[stream] += std::abs(value) >= 0.999999 ? 1 : 0;
            }
        }
        std::this_thread::sleep_until(start + std::chrono::microseconds(first * 1000000 / 2400000));
    }
    for (int stream = 0; stream < 2; ++stream) {
        std::printf("SINGLE_%s %s deviation=2500 tone=1000 iq_amplitude=0.3 "
                    "warmup_frames=24000 samples=%zu peak=%.6f rms=%.6f outside=%zu clipped=%zu\n",
            mode == T::Mode::Fm ? "FM" : "FMN", stream == 0 ? "tap" : "speaker",
            count[stream], peak[stream], std::sqrt(energy[stream] / std::max<std::size_t>(1, count[stream])),
            outside[stream], clipped[stream]);
        check(count[stream] > 20000, "single FM measurement contains settled audio");
        check(outside[stream] == 0 && clipped[stream] == 0,
            "in-range single FM signal produces unclipped normalized audio");
        const double expectedPeak = mode == T::Mode::Fm ? 0.235 : 0.470;
        check(std::abs(peak[stream] - expectedPeak) < 0.025,
            "FM and FMN use distinct full-deviation normalization");
    }
    pipeline->stop();
}
#ifdef AETHER_BACKEND_RTL
static void measureSquelchPipeline()
{
    auto pipeline = std::make_unique<Pipeline>();
    T::State state;
    state.token = {101, 1};
    state.capture = {101, 1, 100000000, 2400000, 1080000, 1080000};
    state.receivers = {{{3, 100000000, -8000, 8000, 0, 3000, 3000}, T::Mode::Fmn,
                        100, 50, false, true, 100}};
    state.receivingIds = {3};
    check(pipeline->prepare(state, true) && ready(*pipeline) && pipeline->adopt(),
        "sparse squelched receiver prepares");
    const auto epoch = AetherSDR::rtl::RtlReceivePipelineTestAccess::receiverEpoch(*pipeline, 3);
    AetherSDR::rtl::RtlSdrDdc ddc;
    ddc.applyCapture(2400000, 100000000, 100000000, T::Mode::Fmn, -8000, 8000);
    ddc.setSpectrumRateFps(1); // display throttling must not chatter the gate
    std::uint64_t first = 0;
    QVector<std::complex<float>> iq(8192);
    std::array<double, 4> tapPeak{}, speakerPeak{};
    std::array<std::size_t, 4> counts{};
    const auto start = std::chrono::steady_clock::now();
    int phase = 0;
    constexpr std::uint64_t kPhaseSamples = 1920000; // 0.8 seconds per state
    while (first < 4 * kPhaseSamples) {
        const int wanted = first / kPhaseSamples;
        if (wanted != phase) {
            phase = wanted;
            state.token.revision++;
            state.receivers[0].squelchEnabled = phase != 3;
            state.receivers[0].squelchLevel = phase == 1 ? 25 : 100;
            check(pipeline->prepare(state) && ready(*pipeline) && pipeline->adopt(),
                "squelch edit adopts without rebuilding the demodulator");
            check(AetherSDR::rtl::RtlReceivePipelineTestAccess::receiverEpoch(*pipeline, 3) == epoch,
                "threshold updates preserve receiver epoch and DSP history");
        }
        for (int i = 0; i < iq.size(); ++i) {
            const double angle = 2.5 * std::sin(2 * std::numbers::pi * 1000 * (first + i) / 2400000.0);
            iq[i] = {float(0.3 * std::cos(angle)), float(0.3 * std::sin(angle))};
        }
        ddc.processIqData(iq, false); // real FFT, no legacy audio or USB
        const auto spectrum = ddc.takeSquelchSpectrum();
        if (!spectrum.empty()) { pipeline->observeSpectrum(spectrum, first); }
        inCallback = true;
        const bool accepted = pipeline->process(first, {iq.constData(), std::size_t(iq.size())});
        inCallback = false;
        check(accepted, "squelch capture block accepted without callback allocation");
        first += iq.size();
        Pipeline::Packet packet;
        while (pipeline->takePacket(packet)) {
            for (std::size_t i = 0; i < packet.frames; ++i) {
                const std::uint64_t frame = packet.firstSample + i;
                const int window = frame / 38400;
                if (window >= 4 || frame % 38400 < 24000 || frame % 38400 > 33600) { continue; }
                const double value = std::abs(packet.samples[2 * i]);
                if (packet.slot == 3) { tapPeak[window] = std::max(tapPeak[window], value); ++counts[window]; }
                else if (packet.slot == -1) { speakerPeak[window] = std::max(speakerPeak[window], value); }
            }
        }
        std::this_thread::sleep_until(start + std::chrono::microseconds(first * 1000000 / 2400000));
    }
    for (int window = 0; window < 4; ++window) {
        check(counts[window] > 8000, "squelch phase contains settled tap samples");
        const bool open = window == 1 || window == 3;
        std::printf("SQL window=%d tap_peak=%.6f speaker_peak=%.6f\n", window, tapPeak[window], speakerPeak[window]);
        check(open ? tapPeak[window] > 0.4 && speakerPeak[window] > 0.4
                   : tapPeak[window] == 0 && speakerPeak[window] == 0,
            "real FFT threshold gates both sparse receiver tap and speaker; Off passes audio");
    }
    pipeline->stop();
}
#endif

static void measureParkAndResume()
{
    auto pipeline = std::make_unique<Pipeline>();
    T::State state;
    state.token = {111, 1};
    state.hardware.centerHz = 100'000'000;
    state.capture = {111, 1, 100'000'000, 2'400'000, 1'080'000, 1'080'000};
    state.receivers = {{{0, 100'000'000, -8000, 8000, 0, 3000, 3000}, T::Mode::Fm}};
    state.receivingIds = {0};
    check(pipeline->prepare(state, true) && ready(*pipeline) && pipeline->adopt(),
          "captured receiver prepares before parking");
    std::array<std::complex<float>, 8192> iq;
    iq.fill({0.25f, 0.0f});
    const auto consume = [&](int expectedRevision, bool expectAudio) {
        bool observed = false;
        for (std::uint64_t first = 0; first < 8192 * 100; first += iq.size()) {
            check(pipeline->process(first, iq), "park/resume capture block accepted");
            Pipeline::Packet packet;
            while (pipeline->takePacket(packet)) {
                check(packet.token.revision == expectedRevision,
                      "audio keeps the adopted capture revision");
                observed = true;
            }
            std::this_thread::sleep_for(3ms);
        }
        check(observed == expectAudio,
              "only a captured receiver emits slice or speaker PCM");
    };
    consume(1, true);

    state.token.revision = 2;
    state.capture.generation = 2;
    state.capture.centerHz = 103'000'000;
    state.hardware.centerHz = 103'000'000;
    state.receivingIds.clear();
    check(pipeline->prepare(state, true) && ready(*pipeline) && pipeline->adopt(),
          "all-parked capture retains a valid empty receiver bank");
    consume(2, false);

    state.token.revision = 3;
    state.capture.generation = 3;
    state.capture.centerHz = 100'000'000;
    state.hardware.centerHz = 100'000'000;
    state.receivingIds = {0};
    check(pipeline->prepare(state, true) && ready(*pipeline) && pipeline->adopt(),
          "returning capture rebuilds the preserved receiver");
    consume(3, true);
    pipeline->stop();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    measureParkAndResume();
    measureSingleFmReceiver(T::Mode::Fm);
    measureSingleFmReceiver(T::Mode::Fmn);
#ifdef AETHER_BACKEND_RTL
    measureSquelchPipeline();
#endif
    {
        auto candidate = std::make_unique<Pipeline>();
        T::State accepted;
        accepted.token = {7, 1};
        accepted.capture = {7, 1, 100000000, 2400000, 1080000, 1080000};
        accepted.receivers = {{{2, 100000000, -8000, 8000, 0, 3000, 3000}, T::Mode::Fm}};
        accepted.receivingIds = {2};
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
        state.receivingIds.push_back(id);
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
        double peak = 0.0;
        double energy = 0.0;
        std::size_t outsidePcmRange = 0;
        for (std::size_t n = 4096; n < audio[id].size(); ++n) {
            const double sample = audio[id][n];
            peak = std::max(peak, std::abs(sample));
            energy += sample * sample;
            outsidePcmRange += std::abs(sample) > 1.0 ? 1 : 0;
        }
        std::printf("FM carrier %d: peak=%.6f rms=%.6f outside_pcm_range=%zu\n",
            id, peak, std::sqrt(energy / std::max<std::size_t>(1, audio[id].size() - 4096)),
            outsidePcmRange);
        // The generated 3.5 kHz deviation is below standard FM's 5 kHz.
        // Its pre-monitor PCM must not require clipping at unity AF gain.
        check(peak <= 1.0, "in-range FM modulation stays within normalized PCM range");
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
