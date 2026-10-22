#include "core/backends/rtl/RtlReceivePipeline.h"
#include "core/backends/rtl/RtlReceiveRecipe.h"
#include "core/backends/rtl/RtlDcBlocker.h"
#include <QCoreApplication>
#include <QThreadPool>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <functional>
#include <fstream>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <numeric>
#include <span>
#include <thread>
#include <string_view>
#include <vector>

using Pipeline = AetherSDR::rtl::RtlReceivePipeline;
using T = AetherSDR::rtl::RtlCaptureTransaction;
using Recipe = AetherSDR::rtl::RtlReceiveRecipe;
using namespace std::chrono_literals;
namespace {
int failures = 0;
void check(bool ok, const char* message)
{ if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); } }
constexpr int kRate = 240000, kChunk = 1200;
constexpr double kCenter = 100000000, kTau = 2 * std::numbers::pi;
bool ready(Pipeline& pipeline)
{
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = pipeline.service();
        if (status != Pipeline::Preparation::Pending) { return status == Pipeline::Preparation::Ready; }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}
T::State state(std::span<const T::Mode> modes)
{
    T::State result;
    result.token = {1, 1};
    result.capture = {1, 1, kCenter, kRate, kRate * .45, kRate * .45};
    result.hardware.centerHz = std::uint32_t(kCenter); result.hardware.sampleRateHz = kRate;
    for (std::size_t id = 0; id < modes.size(); ++id) {
        auto [low, high] = Recipe::defaultFilter(modes[id]);
        if (modes[id] == T::Mode::Fm) { low = -8000; high = 8000; }
        T::Receiver receiver{{int(id), kCenter + 20000, double(low), double(high), 0, 3000, 3000}, modes[id]};
        receiver.agcMode = 0;
        receiver.audioMute = true; // Independent PCM must survive monitor mute.
        result.receivers.push_back(receiver); result.receivingIds.push_back(int(id));
    }
    return result;
}
using Audio = std::array<std::vector<float>, 8>;
using Signal = std::function<std::complex<double>(double)>;
struct Identity { std::uint64_t instance = 0, epoch = 0, capture = 0; bool discontinuity = false; };
struct Run { Audio audio; std::array<Identity, 8> identities{}; std::array<double, 8> stereoError{}; };
Run feed(Pipeline& pipeline, std::uint64_t& position, double seconds, const Signal& signal)
{
    Run result;
    std::array<std::complex<float>, kChunk> iq{};
    const std::uint64_t start = position, end = start + std::uint64_t(seconds * kRate);
    AetherSDR::rtl::RtlDcBlocker dc;
    dc.configure(true, kRate);
    while (position < end) {
        for (std::size_t index = 0; index < iq.size(); ++index) {
            const auto value = signal(double(position + index) / kRate);
            iq[index] = {float(value.real()), float(value.imag())};
        }
        dc.process(iq);
        check(pipeline.process(position, iq), "production acquisition accepts analytic AM capture");
        Pipeline::Packet packet;
        while (pipeline.takePacket(packet)) {
            if (packet.slot < 0 || packet.slot >= 8) { continue; }
            auto& pcm = result.audio[packet.slot];
            auto& identity = result.identities[packet.slot];
            if (!identity.instance) {
                identity = {packet.instance, packet.receiverEpoch, packet.captureEpoch, packet.discontinuity};
            }
            for (std::size_t index = 0; index < packet.frames; ++index) {
                check(std::isfinite(packet.samples[2 * index]), "AM PCM is finite");
                result.stereoError[packet.slot] = std::max(result.stereoError[packet.slot],
                    double(std::abs(packet.samples[2 * index] - packet.samples[2 * index + 1])));
                // Ignore startup/reconfiguration latency and carrier leveler settling.
                if (packet.firstSample + index >= start / 5 + 48000) {
                    pcm.push_back(packet.samples[2 * index]);
                }
            }
        }
        position += kChunk;
        std::this_thread::sleep_for(6ms);
    }
    check(!pipeline.needsRepair(), "AM workload has no receiver withdrawal");
    return result;
}
Run render(const T::State& recipe, double seconds, const Signal& signal)
{
    // The fixed mixer buffers exceed the default Windows thread stack.
    const auto pipelineOwner = std::make_unique<Pipeline>(recipe.receivers.size());
    Pipeline& pipeline = *pipelineOwner;
    if (!pipeline.prepare(recipe, true) || !ready(pipeline) || !pipeline.adopt()) {
        check(false, "AM/SAM bank prepares and adopts"); return {};
    }
    check(!pipeline.legacy(), "AM and SAM use native independent receivers");
    std::uint64_t position = 0;
    Run output = feed(pipeline, position, seconds, signal);
    pipeline.stop(); return output;
}
double magnitude(std::span<const float> pcm, double hz)
{
    if (pcm.size() < 2) { return 0; }
    std::complex<double> sum{}; double weight = 0;
    for (std::size_t index = 0; index < pcm.size(); ++index) {
        const double window = .5 - .5 * std::cos(kTau * index / (pcm.size() - 1));
        sum += double(pcm[index]) * window * std::polar(1.0, -kTau * hz * index / 48000);
        weight += window;
    }
    return 2 * std::abs(sum) / weight;
}
double ratioDb(double a, double b) { return 20 * std::log10(std::max(a, 1e-12) / std::max(b, 1e-12)); }
double energy(std::span<const float> pcm)
{
    double sum = 0;
    for (const float value : pcm) { sum += double(value) * value; }
    return pcm.empty() ? 0 : sum / pcm.size();
}
std::complex<double> am(double time, double carrier, double audio, double amplitude = .04, double modulation = .6)
{ return std::polar(amplitude * (1 + modulation * std::cos(kTau * audio * time)), kTau * carrier * time); }
void fidelityAndPll()
{
    constexpr std::array modes{T::Mode::Am, T::Mode::Sam, T::Mode::Sam, T::Mode::Sam};
    auto recipe = state(modes);
    for (int id = 0; id < 4; ++id) { recipe.receivers[id].passband.carrierHz = kCenter - 60000 + id * 40000; }
    const auto result = render(recipe, 3, [](double time) {
        return am(time, -60000, 700) + am(time, -20000, 1100)
            + am(time, 20000 + 1500, 1300) + am(time, 60000 - 1500, 1700);
    });
    const std::array tones{700., 1100., 1300., 1700.};
    for (int id = 0; id < 4; ++id) {
        const auto& pcm = result.audio[id];
        const double tone = magnitude(pcm, tones[id]);
        const double distortion = ratioDb(tone, magnitude(pcm, tones[id] * 2));
        const double dc = pcm.empty() ? 1 : std::abs(std::accumulate(pcm.begin(), pcm.end(), 0.0) / pcm.size());
        std::printf("AM fidelity receiver=%d tone=%.6f distortion=%.1fdB dc=%.8f frames=%zu\n", id, tone, distortion, dc, pcm.size());
        check(pcm.size() > 48000 && std::abs(tone / (.04 * .6 * std::sqrt(10.0)) - 1) < .05, "AM/SAM deliver useful unclipped calibrated modulation level");
        check(distortion > 35, "AM/SAM second harmonic distortion below -35 dBc including +/-1500 Hz acquisition");
        check(dc < tone * .01, "demodulated carrier DC suppressed below one percent of wanted tone");
        check(result.stereoError[id] < 1e-7, "AM/SAM deliver equal real mono channels without opposite quadrature audio");
        for (int other = 0; other < 4; ++other) {
            if (id != other) { check(ratioDb(tone, magnitude(pcm, tones[other])) > 35, "independent AM/SAM reject other receiver audio"); }
        }
    }
    constexpr std::array lowModes{T::Mode::Am, T::Mode::Sam};
    const auto lowAudio = render(state(lowModes), 3, [](double time) { return am(time, 20000, 150); });
    for (const int id : {0, 1}) {
        check(std::abs(magnitude(lowAudio.audio[id], 150) / (.04 * .6 * std::sqrt(10.0)) - 1) < .08,
            "carrier DC removal preserves wanted low-frequency audio at 150 Hz");
    }
    // A quadrature interferer changes an envelope detector's magnitude but
    // does not belong to the coherent in-phase audio. This fails if SAM is AM.
    constexpr std::array compareModes{T::Mode::Am, T::Mode::Sam};
    const auto coherent = render(state(compareModes), 3, [](double time) {
        return .04 * std::complex<double>(1 + .3 * std::cos(kTau * 700 * time), .5 * std::cos(kTau * 1500 * time))
            * std::polar(1.0, kTau * 20300 * time);
    });
    const double rejection = ratioDb(magnitude(coherent.audio[0], 3000), magnitude(coherent.audio[1], 3000));
    std::printf("SAM quadrature-envelope artifact rejection=%.1fdB\n", rejection);
    check(rejection > 15, "SAM coherently rejects a quadrature artifact that envelope AM demodulates");
    check(magnitude(coherent.audio[1], 700) > .01, "SAM retains real AM modulation while rejecting quadrature artifact");
}
void asymmetricFiltersAndRit()
{
    constexpr std::array modes{T::Mode::Sam, T::Mode::Sam, T::Mode::Sam, T::Mode::Am};
    auto recipe = state(modes);
    recipe.receivers[0].passband.filterLowHz = -500;
    recipe.receivers[1].passband.filterHighHz = 500;
    recipe.receivers[2].ritEnabled = true; recipe.receivers[2].ritOffsetHz = 1200;
    recipe.receivers[2].passband.translationHz = 1200;
    recipe.receivers[2].passband.filterLowHz = -500;
    recipe.receivers[2].passband.filterHighHz = 500;
    recipe.receivers[3].passband.filterLowHz = -500;
    recipe.receivers[3].passband.filterHighHz = 500;
    const auto result = render(recipe, 3, [](double time) {
        return std::polar(.04, kTau * 20000 * time)
            + std::polar(.008, kTau * 20700 * time)
            + std::polar(.008, kTau * 18700 * time)
            + am(time, 21200, 250, .01, .4);
    });
    check(ratioDb(magnitude(result.audio[0], 700), magnitude(result.audio[0], 1300)) > 35,
        "SAM upper RF cut rejects lower-sideband interference");
    check(ratioDb(magnitude(result.audio[1], 1300), magnitude(result.audio[1], 700)) > 35,
        "SAM lower RF cut rejects upper-sideband interference with correct IQ orientation");
    check(magnitude(result.audio[2], 250) > .005, "RIT selects displaced carrier without changing dial");
    check(magnitude(result.audio[3], 700) < .001, "AM narrow RF cuts reject out-of-passband modulation");
}
void fadesRetunesAndEpochs()
{
    constexpr std::array modes{T::Mode::Sam, T::Mode::Am};
    auto recipe = state(modes);
    const auto pipelineOwner = std::make_unique<Pipeline>(2);
    Pipeline& pipeline = *pipelineOwner;
    check(pipeline.prepare(recipe, true) && ready(pipeline) && pipeline.adopt(), "fade fixture adopts");
    std::uint64_t position = 0;
    const auto first = feed(pipeline, position, 2.5, [](double t) { return am(t, 20900, 1000); });
    const double baseline = magnitude(first.audio[0], 1000);
    feed(pipeline, position, 1.5, [](double) { return std::complex<double>{}; });
    const auto recovered = feed(pipeline, position, 3, [](double t) { return am(t, 20900, 1000, .004); });
    const double after = magnitude(recovered.audio[0], 1000);
    std::printf("SAM dropout fade recovered normalized_gain=%.3f\n", after * 10 / baseline);
    check(after * 10 > baseline * .85 && after * 10 < baseline * 1.15,
        "SAM recovers its carrier after complete dropout and 20 dB fade");
    recipe.token.revision++; recipe.receivers[0].passband.carrierHz += 5000;
    check(pipeline.prepare(recipe) && ready(pipeline) && pipeline.adopt(), "SAM retune adopts atomically");
    const auto retuned = feed(pipeline, position, 2.5, [](double t) { return am(t, 25300, 1000); });
    check(magnitude(retuned.audio[0], 1000) > baseline * .85, "retuned SAM acquires fresh PLL");
    check(retuned.identities[0].epoch != first.identities[0].epoch
        && retuned.identities[0].capture != first.identities[0].capture,
        "SAM retune retires receiver and presentation epochs");
    recipe.token.revision++; recipe.receivingIds = {1};
    check(pipeline.prepare(recipe) && ready(pipeline) && pipeline.adopt(), "SAM receiver parks");
    const auto parked = feed(pipeline, position, 1.2, [](double t) { return am(t, 25300, 1000); });
    check(!parked.identities[0].instance, "parked SAM produces no PCM");
    recipe.token.revision++; recipe.receivingIds = {0, 1};
    // Retirement is asynchronous; the public admission result identifies its retry.
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    auto submission = Pipeline::Submission::RetryRetiringSlot;
    while (submission == Pipeline::Submission::RetryRetiringSlot && std::chrono::steady_clock::now() < deadline) {
        submission = pipeline.prepareDetailed(recipe); std::this_thread::sleep_for(1ms);
    }
    check(submission == Pipeline::Submission::Accepted && ready(pipeline) && pipeline.adopt(), "parked SAM recreates");
    const auto resumed = feed(pipeline, position, 2.5, [](double t) { return am(t, 25300, 1000); });
    check(resumed.identities[0].instance != retuned.identities[0].instance
        && resumed.identities[0].discontinuity && magnitude(resumed.audio[0], 1000) > baseline * .85,
        "resumed SAM has a new identity and acquired audio");
    recipe.token.revision++; recipe.receivers[0].mode = T::Mode::Am;
    check(pipeline.prepare(recipe) && ready(pipeline) && pipeline.adopt(), "SAM to AM transition adopts");
    const auto envelope = feed(pipeline, position, 2.5, [](double t) { return am(t, 25300, 1000); });
    check(envelope.identities[0].epoch != resumed.identities[0].epoch
        && magnitude(envelope.audio[0], 1000) > baseline * .85, "AM transition retires SAM DSP and preserves wanted level");
    pipeline.stop();
}
void mixedEight()
{
    constexpr std::array modes{T::Mode::Am, T::Mode::Sam, T::Mode::Usb, T::Mode::Lsb,
        T::Mode::Cw, T::Mode::Cwr, T::Mode::Fm, T::Mode::Sam};
    auto recipe = state(modes);
    constexpr std::array offsets{-84000., -60000., -36000., -12000., 12000., 36000., 60000., 84000.};
    constexpr std::array tones{700., 900., 1100., 1300., 600., 600., 1500., 1700.};
    for (int id = 0; id < 8; ++id) { recipe.receivers[id].passband.carrierHz = kCenter + offsets[id]; }
    const auto result = render(recipe, 3, [&](double t) {
        return am(t, offsets[0], tones[0], .015) + am(t, offsets[1] + 500, tones[1], .015)
            + std::polar(.015, kTau * (offsets[2] + tones[2]) * t)
            + std::polar(.015, kTau * (offsets[3] - tones[3]) * t)
            + std::polar(.015, kTau * offsets[4] * t) + std::polar(.015, kTau * offsets[5] * t)
            + std::polar(.015, kTau * offsets[6] * t + (2500 / tones[6]) * std::sin(kTau * tones[6] * t))
            + am(t, offsets[7] - 500, tones[7], .015);
    });
    for (int id = 0; id < 8; ++id) {
        const double wanted = magnitude(result.audio[id], tones[id]);
        std::printf("mixed receiver=%d mode=%d tone=%.5f frames=%zu\n", id, int(modes[id]), wanted, result.audio[id].size());
        check(result.audio[id].size() > 48000 && wanted > .008, "all eight mixed AM/SAM/SSB/CW/FM receivers demodulate wanted RF");
        for (int other = 0; other < 8; ++other) {
            if (tones[id] != tones[other]) { check(ratioDb(wanted, magnitude(result.audio[id], tones[other])) > 30,
                "mixed analog bank keeps unrelated audio isolated"); }
        }
    }
}
void gainNoiseAndNotches()
{
    constexpr std::array modes{T::Mode::Am, T::Mode::Sam, T::Mode::Sam, T::Mode::Sam};
    auto recipe = state(modes);
    recipe.receivers[1].agcOffLevel = 30;
    recipe.receivers[2].mnEnabled = true;
    recipe.receivers[2].mnLevel = 40; // 1700 Hz, paired sidebands.
    recipe.receivers[3].anfEnabled = true; recipe.receivers[3].anfLevel = 100;
    const auto result = render(recipe, 3, [](double t) { return am(t, 20000, 1700, .01); });
    const double reference = magnitude(result.audio[0], 1700);
    const double gainDb = ratioDb(magnitude(result.audio[1], 1700), reference);
    const double mnDb = ratioDb(reference, magnitude(result.audio[2], 1700));
    const double anfDb = ratioDb(reference, magnitude(result.audio[3], 1700));
    std::printf("AM controls gain=%.2fdB manual_notch=%.1fdB ANF=%.1fdB\n", gainDb, mnDb, anfDb);
    check(std::abs(gainDb - 12) < .6, "AM/SAM share linear level and mapped AGC-off gain");
    check(mnDb > 25, "AM manual notch removes both sidebands of its selected audio tone");
    check(anfDb > 10, "AM automatic notch removes an actual coherent audio tone");

    recipe = state(modes);
    for (auto& rx : recipe.receivers) { rx.agcMode = 4; }
    recipe.receivers[1].agcMode = 2;
    recipe.receivers[2].agcThreshold = 35;
    recipe.receivers[3].agcMode = 0;
    const auto agc = render(recipe, 4.5, [](double t) { return am(t, 20000, 1000, t < 1.5 ? .1 : .0001); });
    const auto level = [&](int id, double first, double last) {
        const auto& pcm = agc.audio[id];
        const std::size_t begin = std::size_t((first - 1) * 48000), count = std::size_t((last - first) * 48000);
        return pcm.size() >= begin + count ? magnitude(std::span(pcm).subspan(begin, count), 1000) : 0;
    };
    const double fast = level(0, 2, 2.3), slow = level(1, 2, 2.3);
    const double late = level(0, 3.9, 4.2), lowThreshold = level(2, 3.9, 4.2);
    std::printf("AM AGC fast_vs_slow=%.1fdB threshold=%.1fdB\n", ratioDb(fast, slow), ratioDb(late, lowThreshold));
    check(ratioDb(fast, slow) > 6, "AM/SAM fast AGC recovers before slow hang");
    check(ratioDb(late, lowThreshold) > 6, "AM/SAM AGC-T changes actual weak-signal gain");
    check(late > level(3, 3.9, 4.2) * 3, "AM AGC raises weak modulation relative to fixed gain");

    recipe = state(modes);
    recipe.receivers[1].nbKind = AetherSDR::NoiseBlankerKind::Impulse;
    recipe.receivers[2].nbKind = AetherSDR::NoiseBlankerKind::Advanced;
    recipe.receivers[2].nbFill = AetherSDR::NoiseBlankerFill::Interpolate;
    recipe.receivers[1].nbLevel = 80; recipe.receivers[2].nbLevel = 80;
    const auto blanked = render(recipe, 3, [](double t) {
        const auto n = std::llround(t * kRate);
        return am(t, 20000, 1000, .002) + std::complex<double>(n % 10000 == 0 ? .5 : 0, 0);
    });
    const auto residual = [](const auto& pcm) {
        const double tone = magnitude(pcm, 1000);
        return std::max(1e-12, energy(pcm) - tone * tone / 2);
    };
    for (const int id : {1, 2}) {
        // Compare SAM blankers with the SAM reference, keeping demodulator identical.
        const double reduction = 10 * std::log10(residual(blanked.audio[3]) / residual(blanked.audio[id]));
        std::printf("AM blanker=%d residual_reduction=%.1fdB\n", id, reduction);
        check(reduction > 3, "AM/SAM receiver blanker reduces actual impulse artifacts");
        check(magnitude(blanked.audio[id], 1000) > magnitude(blanked.audio[3], 1000) * .8,
            "AM blanker preserves desired modulation");
    }
    recipe = state(modes);
    recipe.receivers[2].nrEnabled = true; recipe.receivers[2].nrLevel = 100;
    recipe.receivers[3].mode = T::Mode::Am;
    recipe.receivers[3].nrEnabled = true; recipe.receivers[3].nrLevel = 100;
    std::uint32_t noiseState = 1;
    const auto reduced = render(recipe, 3, [&](double t) {
        const auto noise = [&] {
            noiseState = 1664525u * noiseState + 1013904223u;
            return (double(noiseState) / 4294967296.0 - .5) * .05;
        };
        // Explicit sequencing gives every compiler the same complex noise fixture.
        const double noiseQ = noise();
        const double noiseI = noise();
        return am(t, 20000, 1500, .04, .3) + std::complex<double>(noiseI, noiseQ);
    });
    const auto snr = [](const auto& pcm) {
        const double signal = std::pow(magnitude(pcm, 1500), 2) / 2;
        return signal / std::max(1e-12, energy(pcm) - signal);
    };
    for (const auto [plain, filtered] : {std::pair{0, 3}, std::pair{1, 2}}) {
        const double improvement = 10 * std::log10(snr(reduced.audio[filtered]) / snr(reduced.audio[plain]));
        std::printf("AM NR mode=%d SNR_improvement=%.1fdB\n", plain, improvement);
        check(improvement > 3, "AM/SAM adaptive noise reduction improves actual audio SNR");
        check(magnitude(reduced.audio[filtered], 1500) > magnitude(reduced.audio[plain], 1500) * .25,
            "AM/SAM NR preserves wanted modulation");
    }
    recipe = state(modes);
    recipe.trackingNotches.entries = {{1, kCenter + 20000 + 1700, 400, true},
                                     {2, kCenter + 20000 - 1700, 400, true}};
    const auto tracked = render(recipe, 3, [](double t) { return am(t, 20000, 1700, .01); });
    for (const int id : {0, 1}) {
        check(ratioDb(reference, magnitude(tracked.audio[id], 1700)) > 25,
            "absolute RF tracking notches remove both AM sidebands without a center-frequency shortcut");
    }
}
void refusals()
{
    constexpr std::array modes{T::Mode::Am, T::Mode::Sam};
    const auto good = state(modes);
    const auto pipelineOwner = std::make_unique<Pipeline>(2);
    Pipeline& pipeline = *pipelineOwner;
    for (int invalid = 0; invalid < 6; ++invalid) {
        auto bad = good;
        auto& rx = bad.receivers[0];
        switch (invalid) {
        case 0: rx.passband.filterLowHz = 0; break;
        case 1: rx.passband.filterHighHz = 99; break;
        case 2: rx.passband.filterLowHz = -12001; break;
        case 3: rx.passband.translationHz = 100; break;
        case 4: rx.agcMode = 1; break;
        case 5: rx.agcOffLevel = 101; break;
        }
        check(!pipeline.prepare(bad), "AM/SAM boundary rejects invalid filter/RIT/gain before bank mutation");
    }
    auto full = good;
    full.receivers[0].mnEnabled = true;
    for (int id = 0; id < AetherSDR::rtl::RtlTrackingNotches::kCapacity; ++id) {
        full.trackingNotches.entries.push_back({id, kCenter + 20000 + 500 + id * 100, 200, false});
    }
    check(pipeline.prepare(full) && ready(pipeline) && pipeline.adopt(),
        "paired AM manual notch coexists with the entire advertised tracking notch bank");
    pipeline.stop();
}
int externalFixture(const char* inputPath, const char* outputPath)
{
    std::ifstream input(inputPath, std::ios::binary);
    std::ofstream output(outputPath, std::ios::binary);
    if (!input || !output) { return 2; }
    constexpr std::array modes{T::Mode::Sam};
    auto recipe = state(modes);
    if (const char* mode = std::getenv("AETHER_RTL_AM_MODE"); mode && std::string_view(mode) == "AM") {
        recipe.receivers[0].mode = T::Mode::Am;
    }
    recipe.receivers.resize(1); recipe.receivingIds = {0};
    recipe.receivers[0].passband.carrierHz = kCenter + 40000;
    const auto pipelineOwner = std::make_unique<Pipeline>();
    Pipeline& pipeline = *pipelineOwner;
    if (!pipeline.prepare(recipe, true) || !ready(pipeline) || !pipeline.adopt()) { return 3; }
    std::array<float, kChunk * 2> raw{};
    std::array<std::complex<float>, kChunk> iq{};
    std::uint64_t first = 0, frames = 0;
    while (first < std::uint64_t(kRate) * 300 && input.read(reinterpret_cast<char*>(raw.data()), sizeof(raw))) {
        for (std::size_t index = 0; index < iq.size(); ++index) {
            if (!std::isfinite(raw[2 * index]) || !std::isfinite(raw[2 * index + 1])) { return 4; }
            iq[index] = {raw[2 * index], raw[2 * index + 1]};
        }
        if (!pipeline.process(first, iq)) { return 5; }
        Pipeline::Packet packet;
        while (pipeline.takePacket(packet)) {
            if (packet.slot != 0) { continue; }
            for (std::size_t index = 0; index < packet.frames; ++index) {
                const float sample = packet.samples[index * 2];
                output.write(reinterpret_cast<const char*>(&sample), sizeof(sample));
                ++frames;
            }
        }
        first += iq.size();
        std::this_thread::sleep_for(6ms);
    }
    const bool success = !pipeline.needsRepair() && frames > 24000 && output.good();
    pipeline.stop();
    std::printf("external IQ capture_frames=%llu mono_pcm_frames=%llu sample_rate=48000\n",
        static_cast<unsigned long long>(first), static_cast<unsigned long long>(frames));
    return success ? 0 : 6;
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const char* iq = std::getenv("AETHER_RTL_AM_INPUT_IQ");
    const char* pcm = std::getenv("AETHER_RTL_AM_OUTPUT_PCM");
    if (iq && pcm) { return externalFixture(iq, pcm); }
    fidelityAndPll(); asymmetricFiltersAndRit(); fadesRetunesAndEpochs(); mixedEight(); gainNoiseAndNotches(); refusals();
    QThreadPool::globalInstance()->waitForDone();
    std::printf("rtl_am_sam_pipeline_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
