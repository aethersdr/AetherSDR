#include "core/backends/rtl/RtlReceivePipeline.h"
#include "core/backends/rtl/RtlReceiveRecipe.h"
#include "core/backends/rtl/RtlDcBlocker.h"
#include "models/CwRxModel.h"
#include "core/DecoderPcmAdapter.h"

#include <QCoreApplication>
#include <QThreadPool>
#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <numbers>
#include <span>
#include <thread>
#include <vector>

using Pipeline = AetherSDR::rtl::RtlReceivePipeline;
using T = AetherSDR::rtl::RtlCaptureTransaction;
using Recipe = AetherSDR::rtl::RtlReceiveRecipe;
using namespace std::chrono_literals;
namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
bool ready(Pipeline& pipeline)
{
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto result = pipeline.service();
        if (result != Pipeline::Preparation::Pending) { return result == Pipeline::Preparation::Ready; }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}
constexpr int kRate = 240000;
constexpr int kChunk = 1200;
constexpr double kCenter = 100000000;
constexpr std::array<double, 4> kOffsets{-60000, -20000, 20000, 60000};
T::State state(int rate = kRate)
{
    T::State result;
    result.token = {1, 1};
    result.capture = {1, 1, kCenter, double(rate), rate * .45, rate * .45};
    result.hardware.centerHz = static_cast<std::uint32_t>(kCenter);
    result.hardware.sampleRateHz = rate;
    constexpr std::array<T::Mode, 4> modes{T::Mode::Usb, T::Mode::Lsb, T::Mode::Cw, T::Mode::Cwr};
    for (int id = 0; id < 4; ++id) {
        const auto [low, high] = Recipe::defaultFilter(modes[id]);
        T::Receiver receiver{{id, kCenter + kOffsets[id], double(low), double(high), 0, 3000, 3000}, modes[id]};
        receiver.agcMode = 0;
        receiver.audioMute = id == 2; // Decoder tap must survive speaker mute.
        result.receivers.push_back(receiver);
        result.receivingIds.push_back(id);
    }
    return result;
}
struct Tone { double hz; double amplitude; };
using Audio = std::array<std::vector<float>, 4>;
Audio render(T::State recipe, const std::vector<Tone>& tones, double seconds = 1.6,
             double noise = 0.0, double impulse = 0.0, bool agcStep = false, bool quantize = false)
{
    // The fixed mixer buffers exceed the default Windows thread stack.
    const auto pipelineOwner = std::make_unique<Pipeline>(4);
    Pipeline& pipeline = *pipelineOwner;
    Audio audio;
    if (!pipeline.prepare(recipe, true) || !ready(pipeline) || !pipeline.adopt()) {
        check(false, "real sideband receiver bank prepares and adopts"); return audio;
    }
    check(!pipeline.legacy(), "SSB/CW must never fall back to legacy demodulation");
    const int rate = static_cast<int>(recipe.capture.achievedSampleRateHz);
    const int chunk = rate / 200; // Five milliseconds at either capture rate.
    std::vector<std::complex<float>> iq(chunk);
    AetherSDR::rtl::RtlDcBlocker dc;
    dc.configure(true, rate);
    const std::uint64_t end = static_cast<std::uint64_t>(seconds * rate);
    std::uint32_t random = 0x146713;
    const auto white = [&]() {
        random = random * 1664525u + 1013904223u;
        return (double(random) / 4294967296.0 - .5) * noise;
    };
    for (std::uint64_t first = 0; first < end; first += chunk) {
        for (std::size_t index = 0; index < iq.size(); ++index) {
            std::complex<double> sample{};
            for (const Tone& tone : tones) {
                const double phase = 2 * std::numbers::pi * tone.hz * double(first + index) / rate;
                sample += std::polar(tone.amplitude, phase);
            }
            if (agcStep && first + index < std::uint64_t(rate)) { sample *= 1000.0; }
            sample += std::complex<double>(white(), white());
            // A single-sample real impulse is broadband. Rectangular multi-
            // sample pulses have RF spectral nulls and bias receiver comparisons.
            if (first + index >= std::uint64_t(rate) && (first + index) % 10000 == 0) { sample += impulse; }
            if (quantize) {
                const auto adc = [](double value) {
                    const double code = std::round(std::clamp(value * 127.5 + 127.5, 0.0, 255.0));
                    return (code - 127.5) / 127.5;
                };
                sample = {adc(sample.real()), adc(sample.imag())};
            }
            iq[index] = {float(sample.real()), float(sample.imag())};
        }
        dc.process(iq);
        check(pipeline.process(first, iq), "production acquisition processes generated IQ");
        Pipeline::Packet packet;
        while (pipeline.takePacket(packet)) {
            if (packet.slot < 0 || packet.slot >= 4) { continue; }
            for (std::size_t index = 0; index < packet.frames; ++index) {
                const float value = packet.samples[index * 2];
                check(std::isfinite(value), "demodulated PCM is finite");
                if (packet.firstSample + index >= 24000) { audio[packet.slot].push_back(value); }
            }
        }
        // Pace the real asynchronous WDSP worker; burst feeding tests its ring
        // underrun rather than its RF transfer function.
        std::this_thread::sleep_for(6ms);
    }
    check(!pipeline.needsRepair(), "generated-IQ workload has no receiver withdrawal");
    pipeline.stop();
    return audio;
}
double magnitude(std::span<const float> samples, double hz)
{
    if (samples.empty()) { return 0; }
    // A Hann window separates image rejection from non-integral record leakage.
    std::complex<double> sum{};
    double weight = 0;
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const double window = .5 - .5 * std::cos(2 * std::numbers::pi * index / (samples.size() - 1));
        const double phase = -2 * std::numbers::pi * hz * index / 48000;
        sum += double(samples[index]) * window * std::polar(1.0, phase);
        weight += window;
    }
    return 2 * std::abs(sum) / weight;
}
double rms(std::span<const float> samples)
{
    double power = 0;
    for (float sample : samples) { power += double(sample) * sample; }
    return samples.empty() ? 0 : std::sqrt(power / samples.size());
}
void sidebandsAndCw()
{
    const auto audio = render(state(), {
        {kOffsets[0] + 1000, .015}, {kOffsets[0] - 1700, .015},
        {kOffsets[1] - 1300, .015}, {kOffsets[1] + 1900, .015},
        {kOffsets[2], .015}, {kOffsets[2] + 2000, .015},
        {kOffsets[3], .015}, {kOffsets[3] - 2000, .015}});
    constexpr std::array<double, 4> wanted{1000, 1300, 600, 600};
    constexpr std::array<double, 4> rejected{1700, 1900, 2600, 2600};
    for (int id = 0; id < 4; ++id) {
        const double signal = magnitude(audio[id], wanted[id]);
        const double image = magnitude(audio[id], rejected[id]);
        const double rejection = 20 * std::log10(signal / std::max(image, 1e-12));
        std::printf("receiver=%d tone=%.0fHz wanted=%.6f image=%.6f rejection=%.1fdB frames=%zu\n",
            id, wanted[id], signal, image, rejection, audio[id].size());
        check(audio[id].size() > 24000 && signal > .005, "wanted RF produces expected audio tone on each independent tap");
        check(rejection > 40, "opposite sideband or out-of-passband carrier rejection exceeds 40 dB");
    }
}
void hardwareCaptureRate()
{
    // Exercise the 50:1 RF decimator used by the physical 2.4 MS/s capture,
    // including opposite RF offsets and BFO signs. Equal RF amplitudes must
    // produce equal PCM levels regardless of the capture sample rate.
    std::array<double, 4> reference{};
    constexpr std::array<double, 4> wanted{1000, 1300, 600, 600};
    constexpr std::array<double, 4> rejected{1700, 1900, 2600, 2600};
    for (int rate : {kRate, 2400000}) {
        auto recipe = state(rate);
        const double scale = double(rate) / kRate;
        std::array<double, 4> offsets{};
        for (int id = 0; id < 4; ++id) {
            offsets[id] = kOffsets[id] * scale;
            recipe.receivers[id].passband.carrierHz = kCenter + offsets[id];
        }
        const auto audio = render(recipe, {
            {offsets[0] + 1000, .015}, {offsets[0] - 1700, .015},
            {offsets[1] - 1300, .015}, {offsets[1] + 1900, .015},
            {offsets[2], .015}, {offsets[2] + 2000, .015},
            {offsets[3], .015}, {offsets[3] - 2000, .015}});
        for (int id = 0; id < 4; ++id) {
            const double signal = magnitude(audio[id], wanted[id]);
            const double image = magnitude(audio[id], rejected[id]);
            const double rejection = 20 * std::log10(signal / std::max(image, 1e-12));
            std::printf("capture_rate=%d receiver=%d wanted=%.6f rejection=%.1fdB\n",
                rate, id, signal, rejection);
            check(audio[id].size() > 24000 && signal > .005,
                "hardware-rate capture produces expected SSB/CW audio on all four taps");
            check(rejection > 40, "hardware-rate SSB/CW rejects opposite or out-of-band RF");
            if (rate == kRate) { reference[id] = signal; }
            else {
                const double deltaDb = 20 * std::log10(signal / std::max(reference[id], 1e-12));
                check(std::abs(deltaDb) < .5, "50:1 RF extraction preserves amplitude within half a dB");
            }
        }
    }

    auto recipe = state(2400000);
    constexpr std::array<int, 4> thresholds{45, 65, 100, 45};
    for (int id = 0; id < 4; ++id) {
        auto& receiver = recipe.receivers[id];
        receiver.mode = T::Mode::Usb;
        receiver.passband.carrierHz = kCenter + 400000;
        receiver.passband.filterLowHz = 100;
        receiver.passband.filterHighHz = 2900;
        receiver.agcMode = id == 3 ? 0 : 4;
        receiver.agcThreshold = thresholds[id];
    }
    // A -80 dBFS wanted carrier probes the gain ceiling, below compression at
    // the user-reported 45 setting. Wideband noise dithers the real RTL byte
    // quantization; all chains receive the same quantized acquisition samples.
    const auto audio = render(recipe, {{401500, .0001}}, 2.0, .02, 0, false, true);
    std::array<double, 4> signal{};
    for (int id = 0; id < 4; ++id) {
        signal[id] = magnitude(audio[id], 1500);
        std::printf("weak_input capture_rate=2400000 AGC=%s threshold=%d tone=%.7f rms=%.7f\n",
            id == 3 ? "off" : "fast", thresholds[id], signal[id], rms(audio[id]));
        check(signal[id] > .0001, "weak input survives hardware-rate SSB extraction and gain");
    }
    const double gainStep = 20 * std::log10(signal[1] / std::max(signal[0], 1e-12));
    check(std::abs(gainStep - 12) < .5, "AGC 45 to 65 supplies the mapped 12 dB weak-signal gain");
    check(signal[2] > signal[1] * 3, "upper AGC range raises a weak hardware-rate signal");
    check(signal[0] > signal[3] * 3, "AGC at 45 raises a weak signal above fixed off gain");
}
void pitchRitAndFilter()
{
    auto recipe = state();
    recipe.receivers[0].passband.filterHighHz = 800;
    recipe.receivers[1].passband.filterLowHz = -800;
    recipe.receivers[2].cwPitchHz = 900;
    recipe.receivers[3].cwPitchHz = 400;
    for (int id : {2, 3}) {
        recipe.receivers[id].ritEnabled = true;
        recipe.receivers[id].ritOffsetHz = 100;
        recipe.receivers[id].passband.translationHz = 100;
        recipe.receivers[id].passband.filterLowHz = -100;
        recipe.receivers[id].passband.filterHighHz = 100;
    }
    const auto audio = render(recipe, {
        {kOffsets[0] + 500, .015}, {kOffsets[0] + 1800, .015},
        {kOffsets[1] - 500, .015}, {kOffsets[1] - 1800, .015},
        {kOffsets[2] + 125, .015}, {kOffsets[3] + 125, .015}});
    for (int id : {0, 1}) {
        const double rejection = 20 * std::log10(magnitude(audio[id], 500)
            / std::max(magnitude(audio[id], 1800), 1e-12));
        check(rejection > 40, "operator filter cuts reject an in-sideband unwanted tone");
    }
    // Receiver is shifted +100 Hz by RIT; the remaining +25 Hz RF detune
    // raises CW pitch and lowers reverse-CW pitch.
    check(magnitude(audio[2], 925) > .005, "CW pitch and RIT preserve carrier-centered passband geometry");
    check(magnitude(audio[3], 375) > .005, "CWR pitch has the reverse RF-detune slope");
    check(magnitude(audio[2], 925) > 4 * magnitude(audio[2], 900), "CW detune is not lost at the marker");
    check(magnitude(audio[3], 375) > 4 * magnitude(audio[3], 400), "CWR detune is not lost at the marker");
}
void gainAndPeaking()
{
    auto recipe = state();
    for (auto& receiver : recipe.receivers) {
        receiver.mode = T::Mode::Cw;
        receiver.passband.filterLowHz = -500;
        receiver.passband.filterHighHz = 500;
    }
    recipe.receivers[1].agcOffLevel = 30; // 12 dB above receiver zero.
    recipe.receivers[2].apfEnabled = true;
    recipe.receivers[2].apfLevel = 100;
    recipe.receivers[3].apfEnabled = false;
    const auto audio = render(recipe, {
        {kOffsets[0], .003}, {kOffsets[1], .003},
        {kOffsets[2], .003}, {kOffsets[2] + 250, .003},
        {kOffsets[3], .003}, {kOffsets[3] + 250, .003}});
    const double gainDb = 20 * std::log10(rms(audio[1]) / std::max(rms(audio[0]), 1e-12));
    std::printf("AGC-off level step measured=%.2fdB\n", gainDb);
    check(std::abs(gainDb - 12) < .5, "accepted off-level recipe changes gain by the mapped 12 dB");
    const double peakedRatio = magnitude(audio[2], 600) / std::max(magnitude(audio[2], 850), 1e-12);
    const double plainRatio = magnitude(audio[3], 600) / std::max(magnitude(audio[3], 850), 1e-12);
    std::printf("APF relative selectivity=%.2fdB\n", 20 * std::log10(peakedRatio / plainRatio));
    check(peakedRatio > 2 * plainRatio, "APF changes actual demodulated tone selectivity independently");
}
void agcResponse()
{
    auto recipe = state();
    for (auto& receiver : recipe.receivers) {
        receiver.mode = T::Mode::Usb;
        receiver.passband.filterLowHz = 100; receiver.passband.filterHighHz = 2900;
        receiver.agcMode = 4;
    }
    recipe.receivers[1].agcMode = 2;
    recipe.receivers[2].agcThreshold = 35;
    recipe.receivers[3].agcMode = 0;
    std::vector<Tone> tones;
    for (double carrier : kOffsets) { tones.push_back({carrier + 1500, .0001}); }
    const auto audio = render(recipe, tones, 4.5, 0, 0, true);
    // render discards the first half-second. Leave a generous gap after the
    // strong-to-weak edge for RF/DSP latency; fast AGC has recovered while the
    // slow mode's two-second hang must still hold its reduced gain.
    const auto level = [&](int id, double first, double last) {
        const std::size_t begin = std::size_t((first - .5) * 48000);
        const std::size_t count = std::size_t((last - first) * 48000);
        if (audio[id].size() < begin + count) { return 0.0; }
        return magnitude(std::span(audio[id]).subspan(begin, count), 1500);
    };
    const double earlyFast = level(0, 1.6, 1.9), earlySlow = level(1, 1.6, 1.9);
    const double lateFast = level(0, 3.9, 4.2), lateSlow = level(1, 3.9, 4.2);
    const double lowerThreshold = level(2, 3.9, 4.2), off = level(3, 3.9, 4.2);
    const double modeDb = 20 * std::log10(earlyFast / std::max(earlySlow, 1e-12));
    const double recoveryDb = 20 * std::log10(lateSlow / std::max(earlySlow, 1e-12));
    const double thresholdDb = 20 * std::log10(lateFast / std::max(lowerThreshold, 1e-12));
    std::printf("AGC fast_vs_slow_hang=%.1f dB slow_recovery=%.1f dB threshold_difference=%.1f dB\n",
        modeDb, recoveryDb, thresholdDb);
    check(modeDb > 6, "fast AGC recovers earlier than slow AGC after strong interference");
    check(recoveryDb > 6, "slow AGC recovers after its hang and decay");
    check(thresholdDb > 6, "AGC threshold changes actual weak-signal gain");
    check(lateFast > off * 3, "AGC enabled raises weak-signal gain relative to fixed off gain");
}

double energy(std::span<const float> samples)
{
    double sum = 0;
    for (float sample : samples) { sum += double(sample) * sample; }
    return samples.empty() ? 0 : sum / samples.size();
}
void noiseAndNotches()
{
    auto recipe = state();
    for (auto& receiver : recipe.receivers) {
        receiver.mode = T::Mode::Usb;
        receiver.passband.filterLowHz = 100; receiver.passband.filterHighHz = 2900;
    }
    recipe.receivers[1].mnEnabled = true; recipe.receivers[1].mnLevel = 50; // 1500 Hz
    recipe.receivers[2].anfEnabled = true; recipe.receivers[2].anfLevel = 100;
    recipe.receivers[3].nrEnabled = true; recipe.receivers[3].nrLevel = 100;
    std::vector<Tone> tones;
    for (double carrier : kOffsets) { tones.push_back({carrier + 1500, .02}); }
    const auto audio = render(recipe, tones, 3.0, .05);
    const double reference = magnitude(audio[0], 1500);
    const double mnDb = 20 * std::log10(reference / magnitude(audio[1], 1500));
    const double anfDb = 20 * std::log10(reference / magnitude(audio[2], 1500));
    const auto snr = [](const auto& pcm) {
        const double tone = magnitude(pcm, 1500);
        const double signal = tone * tone / 2;
        return signal / std::max(1e-12, energy(pcm) - signal);
    };
    const double improvement = 10 * std::log10(snr(audio[3]) / snr(audio[0]));
    std::printf("DSP manual_notch=%.1f dB auto_notch=%.1f dB NR_SNR_improvement=%.1f dB\n", mnDb, anfDb, improvement);
    check(mnDb > 25, "manual notch position removes the real RF tone");
    check(anfDb > 10, "automatic notch suppresses a coherent interferer");
    check(improvement > 3, "adaptive noise reduction improves tone SNR");
    check(magnitude(audio[3], 1500) > reference * .25, "NR preserves the wanted coherent signal");

    recipe.receivers[1].mnEnabled = false;
    recipe.receivers[2].anfEnabled = false;
    recipe.receivers[3].nrEnabled = false;
    recipe.receivers[1].nbKind = AetherSDR::NoiseBlankerKind::Impulse;
    recipe.receivers[2].nbKind = AetherSDR::NoiseBlankerKind::Advanced;
    recipe.receivers[2].nbFill = AetherSDR::NoiseBlankerFill::Interpolate;
    recipe.receivers[1].nbLevel = 80; recipe.receivers[2].nbLevel = 80;
    // Identical RF geometry is essential: every chain must see the same
    // interference amplitude and phase before comparing blanker performance.
    for (auto& receiver : recipe.receivers) { receiver.passband.carrierHz = kCenter + 20000; }
    const auto blanked = render(recipe, {{21500, .002}}, 2.6, 0, .5);
    const auto residual = [](const auto& pcm) {
        const double tone = magnitude(pcm, 1500);
        return std::max(1e-12, energy(pcm) - tone * tone / 2);
    };
    for (int id : {1, 2}) {
        const double db = 10 * std::log10(residual(blanked[0]) / residual(blanked[id]));
        std::printf("DSP blanker=%d impulse_energy_reduction=%.1f dB\n", id, db);
        check(db > 3, "real noise blanker reduces impulsive RF interference");
        check(magnitude(blanked[id], 1500) > magnitude(blanked[0], 1500) * .8,
            "noise blanker preserves wanted signal while removing impulses");
    }
}

void trackingNotches()
{
    auto recipe = state();
    const double carrier = kCenter + 20000;
    for (auto& receiver : recipe.receivers) { receiver.passband.carrierHz = carrier; }
    const std::vector<Tone> tones{{20000 + 1500, .015}, {20000 - 1500, .015}, {20000, .015}};
    const auto baseline = render(recipe, tones);
    recipe.trackingNotches.entries = {{7, carrier + 1500, 400, true},
        {19, carrier - 1500, 400, true}, {30, carrier, 400, true}};
    const auto blocked = render(recipe, tones);
    constexpr std::array<double, 4> pitches{1500, 1500, 600, 600};
    for (int id = 0; id < 4; ++id) {
        const double db = 20 * std::log10(magnitude(baseline[id], pitches[id])
            / std::max(1e-12, magnitude(blocked[id], pitches[id])));
        std::printf("tracking_notch receiver=%d rejection=%.1f dB\n", id, db);
        check(db > 25, "absolute RF tracking notch suppresses USB/LSB/CW/CWR interferer");
    }
    recipe.receivers[0].passband.carrierHz += 300;
    recipe.receivers[1].passband.carrierHz -= 300;
    for (int id : {2, 3}) {
        recipe.receivers[id].cwPitchHz = 900;
        recipe.receivers[id].ritEnabled = true; recipe.receivers[id].ritOffsetHz = 100;
        recipe.receivers[id].passband.translationHz = 100;
    }
    const auto tracked = render(recipe, tones);
    constexpr std::array<double, 4> shiftedPitch{1200, 1200, 800, 1000};
    for (int id = 0; id < 4; ++id) {
        check(magnitude(tracked[id], shiftedPitch[id]) < magnitude(baseline[id], pitches[id]) * .056,
            "tracking notch stays on absolute RF after tuning, RIT and CW pitch changes");
    }
    recipe = state();
    for (auto& receiver : recipe.receivers) { receiver.passband.carrierHz = carrier; }
    recipe.trackingNotches.entries = {{7, carrier + 1500, 400, true},
        {19, carrier - 1500, 400, true}, {30, carrier, 400, true}};
    recipe.trackingNotches.enabled = false;
    recipe.receivers[0].mnEnabled = true; recipe.receivers[0].mnLevel = 50;
    const auto bypassed = render(recipe, tones);
    check(magnitude(bypassed[0], 1500) < magnitude(baseline[0], 1500) * .056,
        "global tracking bypass does not disable the independent manual notch");
    for (int id : {1, 2, 3}) {
        const double ratio = magnitude(bypassed[id], pitches[id]) / magnitude(baseline[id], pitches[id]);
        check(ratio > .8 && ratio < 1.2, "global tracking bypass restores each unaffected receiver");
    }
}

void morseThroughRadioDsp()
{
    using namespace AetherSDR;
    // Carrier keying encodes twelve Vs (...-) at 20 WPM. The input contains
    // no audio-frequency tone: the real receiver BFO must create that tone.
    std::vector<float> envelope(kRate / 2, 0);
    const auto silence = [&](int units) { envelope.resize(envelope.size() + units * kRate * 3 / 50); };
    for (int letter = 0; letter < 12; ++letter) {
        if (letter) { silence(letter % 3 ? 3 : 7); }
        for (int mark = 0; mark < 4; ++mark) {
            if (mark) { silence(1); }
            const int count = (mark == 3 ? 3 : 1) * kRate * 3 / 50;
            for (int index = 0; index < count; ++index) {
                const double edge = std::min({1.0, index / (.005 * kRate), (count - 1 - index) / (.005 * kRate)});
                envelope.push_back(float(.025 * edge));
            }
        }
    }
    silence(35);
    envelope.resize((envelope.size() + kChunk - 1) / kChunk * kChunk);
    auto recipe = state();
    recipe.receivers = {recipe.receivers[2]};
    recipe.receivers[0].passband.stableId = 0;
    recipe.receivers[0].audioMute = true;
    recipe.receivingIds = {0};
    const auto pipelineOwner = std::make_unique<Pipeline>();
    Pipeline& pipeline = *pipelineOwner;
    if (!pipeline.prepare(recipe, true) || !ready(pipeline) || !pipeline.adopt()) {
        check(false, "CW Morse production bank adopts"); return;
    }
    CwRxModel decoder;
    QString decoded;
    QObject::connect(&decoder, &CwRxModel::textDecoded, &decoder,
        [&](const QString& text, float) { decoded += text; });
    decoder.setPitchRange(550, 650); decoder.setSpeedRange(18, 22); decoder.start();
    PcmProducer producer;
    check(producer.start(PcmPurpose::Slice, 0, {48000, PcmLayout::Stereo}), "native CW PCM producer starts");
    DecoderPcmAdapter adapter;
    adapter.selectRoute(DecoderPcmAdapter::RouteLane::NativeSlice, 0);
    std::array<std::complex<float>, kChunk> iq;
    for (std::size_t first = 0; first < envelope.size(); first += kChunk) {
        for (std::size_t index = 0; index < iq.size(); ++index) {
            const double phase = 2 * std::numbers::pi * kOffsets[2] * (first + index) / kRate;
            iq[index] = {envelope[first + index] * float(std::cos(phase)),
                         envelope[first + index] * float(std::sin(phase))};
        }
        check(pipeline.process(first, iq), "keyed RF capture processes through real WDSP");
        Pipeline::Packet packet;
        while (pipeline.takePacket(packet)) {
            if (packet.slot != 0) { continue; }
            QVector<float> samples(qsizetype(packet.frames * 2));
            std::copy_n(packet.samples.begin(), samples.size(), samples.begin());
            const auto frame = producer.produce(std::move(samples));
            if (!frame) { check(false, "demodulated CW frame is valid"); continue; }
            const auto block = adapter.accept(*frame);
            if (block) { decoder.feedFixed24(*block); }
        }
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(6ms);
    }
    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(); std::this_thread::sleep_for(2ms);
    }
    std::printf("CW generated V train decoded=\"%s\" pitch=%.1f speed=%.1f\n",
        decoded.toUtf8().constData(), decoder.estimatedPitch(), decoder.estimatedSpeed());
    check(decoded.count('V') >= 3, "actual RTL IQ to WDSP to 48/24k adapter to ggmorse decodes at least three expected Vs");
    check(decoder.estimatedPitch() > 550 && decoder.estimatedPitch() < 650,
        "ggmorse estimates the receiver's real CW BFO tone");
    check(!pipeline.needsRepair(), "Morse decode has no DSP withdrawal despite muted speaker");
    decoder.stop(); pipeline.stop();
}

void refusals()
{
    const auto pipelineOwner = std::make_unique<Pipeline>(4);
    Pipeline& pipeline = *pipelineOwner;
    const auto good = state();
    auto badNotch = good;
    badNotch.trackingNotches.entries = {{1, kCenter, 199, true}};
    check(!pipeline.prepare(badNotch), "tracking width below the real WDSP resolution is refused");
    badNotch.trackingNotches.entries = {{1, kCenter, 200, true}, {1, kCenter + 2000, 200, true}};
    check(!pipeline.prepare(badNotch), "duplicate tracking IDs are refused before bank preparation");
    for (int invalid = 0; invalid < 8; ++invalid) {
        auto bad = good;
        auto& receiver = bad.receivers[0];
        switch (invalid) {
        case 0: receiver.agcMode = 9; break;
        case 1: receiver.agcThreshold = 101; break;
        case 2: receiver.cwPitchHz = 0; break;
        case 3: receiver.passband.filterLowHz = -10; break;
        case 4: receiver.ritOffsetHz = 100000; break;
        case 5: receiver.nbLevel = -1; break;
        case 6: receiver.apfLevel = 101; break;
        case 7: receiver.mode = T::Mode::Am; break;
        }
        check(!pipeline.prepare(bad), "invalid controls and carrier-excluding AM filters are rejected before preparation");
    }
}
// Optional integration fixture: interleaved little-endian float32 analytic IQ
// at 240 kHz, USB carrier +40 kHz from capture center. Produces mono float32
// 48 kHz PCM from the real independent receiver tap. The caller supplies speech
// as analytic upper-sideband IQ and may feed this file to the existing ASR test.
int externalFixture(const char* inputPath, const char* outputPath)
{
    std::ifstream input(inputPath, std::ios::binary);
    std::ofstream output(outputPath, std::ios::binary);
    if (!input || !output) { return 2; }
    auto recipe = state();
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
} // namespace
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const char* iq = std::getenv("AETHER_RTL_SSB_INPUT_IQ");
    const char* pcm = std::getenv("AETHER_RTL_SSB_OUTPUT_PCM");
    if (iq && pcm) { return externalFixture(iq, pcm); }
    sidebandsAndCw();
    hardwareCaptureRate();
    pitchRitAndFilter();
    gainAndPeaking();
    agcResponse();
    noiseAndNotches();
    trackingNotches();
    morseThroughRadioDsp();
    refusals();
    QThreadPool::globalInstance()->waitForDone();
    std::printf("rtl_ssb_cw_pipeline_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
