// Offline SSB peak-to-average harness (RFC #4836, Stage 0).
//
// Runs transmit audio through the real TX voice processor twice -- every
// stage off, then with the settings given on the command line -- and measures
// the 24 kHz transport output the radio would receive: through a modeled radio
// sideband filter (linear and minimum phase), and with --hl2 1 through the
// real Hermes-Lite 2 modulator, whose IQ envelope is measured directly.
// Nothing is keyed and no radio or socket is opened.
//
//   tx_papr_harness <voice-48k.wav | synthetic> [--key value ...]
//
// Input: 48 kHz PCM16 or float32 WAV, mono or stereo (left used), or the word
// `synthetic` for a deterministic 12 s voiced-speech stand-in (jittered
// glottal pulses through three formant resonators, with pauses), so a run is
// reproducible without a recording.
//
// General:     --low 300 --high 2700   radio passband (Hz)
//              --gain 0                dB applied to the input before both runs
//              --hl2 0|1               also measure through the HL2 modulator
//              --hl2-alc 0|1           HL2 ALC in that measurement (default 0)
// EQ:          --eq-hpf 80             a 24 dB/oct high-pass band
//              --eq-p1-freq 500 --eq-p1-gain 2 --eq-p1-q 1   peaking bands p1..p4
// Compressor:  --comp-threshold -20 --comp-ratio 4 --comp-attack 20
//              --comp-release 150 --comp-knee 6 --comp-makeup 5
//              --comp-drive 0 --comp-phase 0 --comp-ceiling -1 (its limiter)
// Tube:        --tube-drive 6 --tube-model 0|1|2 --tube-tone 0 --tube-bias 0
//              --tube-drywet 0.4 --tube-output 0
// Exciter:     --exciter-air 6 --exciter-mix 0.2 --exciter-low-mix 0
//              --exciter-mode 0|1      0 = Even (Aphex), 1 = Odd (Behringer)
// Final:       --lim-ceiling -0.5 (Final Output LIM) --lim-lookahead 1.5 (ms)
//              --trim 0
//
// Any flag of a stage turns that stage on in the processed run; the rest keep
// the stage's own defaults. An unknown flag is an error, so a typo cannot
// switch a stage on with defaults behind the operator's back. Stage order is the shipped default: Tube, EQ,
// Compressor, Exciter.
//
// -DTX_PAPR_HARNESS_NO_HL2 builds it without the HL2 modulator (and so
// without WDSP or the rest of aethercore): TxVoiceProcessor, the Client*
// stages, r8brain and RNNoise are all it needs. That is how the Pi-class CPU
// gate is measured on a board that cannot build the whole tree.

#include "core/ClientComp.h"
#include "core/ClientEq.h"
#include "core/ClientFinalLimiter.h"
#include "core/ClientPudu.h"
#include "core/ClientTube.h"
#include "core/TxChainLayout.h"
#include "core/TxVoiceProcessor.h"
#include "core/dsp/SsbEnvelopeMeter.h"
#ifndef TX_PAPR_HARNESS_NO_HL2
#include "core/backends/TxAudioSource.h"
#include "core/backends/hl2/Hl2TxDsp.h"
#include "TxTestAuthority.h"
#endif

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace AetherSDR;

namespace {

bool readWav48(const char* path, std::vector<float>& mono, std::string& error)
{
    QFile file(QString::fromLocal8Bit(path));
    if (!file.open(QIODevice::ReadOnly)) {
        error = "cannot open input";
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.size() < 12 || std::memcmp(bytes.constData(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.constData() + 8, "WAVE", 4) != 0) {
        error = "not a RIFF/WAVE file";
        return false;
    }
    auto u16 = [&](qsizetype at) {
        return static_cast<uint16_t>(static_cast<uint8_t>(bytes[at]) |
                                     static_cast<uint8_t>(bytes[at + 1]) << 8);
    };
    auto u32 = [&](qsizetype at) {
        return static_cast<uint32_t>(u16(at)) | static_cast<uint32_t>(u16(at + 2)) << 16;
    };
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    qsizetype dataAt = -1;
    uint32_t dataSize = 0;
    for (qsizetype at = 12; at + 8 <= bytes.size();) {
        const uint32_t size = u32(at + 4);
        if (std::memcmp(bytes.constData() + at, "fmt ", 4) == 0 && size >= 16) {
            format = u16(at + 8);
            channels = u16(at + 10);
            rate = u32(at + 12);
            bits = u16(at + 22);
            if (format == 0xFFFE && size >= 40) {
                format = u16(at + 32); // extensible
            }
        } else if (std::memcmp(bytes.constData() + at, "data", 4) == 0) {
            dataAt = at + 8;
            dataSize = static_cast<uint32_t>(std::min<qsizetype>(size, bytes.size() - dataAt));
        }
        at += 8 + size + (size & 1);
    }
    if (dataAt < 0 || channels == 0) {
        error = "no fmt or data chunk";
        return false;
    }
    if (rate != 48000) {
        error = "sample rate is not 48000 Hz (resample first)";
        return false;
    }
    const bool pcm16 = format == 1 && bits == 16;
    const bool float32 = format == 3 && bits == 32;
    if (!pcm16 && !float32) {
        error = "only PCM16 and float32 are supported";
        return false;
    }

    const std::size_t frameBytes = static_cast<std::size_t>(channels) * (bits / 8);
    const std::size_t frames = dataSize / frameBytes;
    mono.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const char* p = bytes.constData() + dataAt + i * frameBytes;
        if (pcm16) {
            int16_t s;
            std::memcpy(&s, p, 2);
            mono[i] = static_cast<float>(s) / 32768.0f;
        } else {
            std::memcpy(&mono[i], p, 4);
        }
    }
    return true;
}

std::vector<float> synthesizeVoice(double seconds)
{
    constexpr double kFs = 48000.0;
    constexpr double kPi = 3.14159265358979323846;
    const double formants[5][3] = {{730, 1090, 2440},
                                   {270, 2290, 3010},
                                   {530, 1840, 2480},
                                   {300, 870, 2240},
                                   {660, 1720, 2410}};
    const double bandwidths[3] = {80, 100, 140};
    const double weights[3] = {1.0, 0.6, 0.35};
    // xorshift64, not <random>: the standard distributions are
    // implementation-defined, and the same input must reach every platform.
    std::uint64_t rngState = 0x9E3779B97F4A7C15ULL;
    auto uniform = [&rngState]() {
        rngState ^= rngState << 13;
        rngState ^= rngState >> 7;
        rngState ^= rngState << 17;
        return static_cast<double>(rngState >> 11) * 0x1.0p-53 - 0.5; // [-0.5, 0.5)
    };

    const std::size_t total = static_cast<std::size_t>(seconds * kFs);
    const std::size_t syllable = static_cast<std::size_t>(0.22 * kFs);
    std::vector<double> out(total);
    double state[3][2] = {};
    double phase = 0.0, envelope = 0.0, peak = 0.0;
    for (std::size_t n = 0; n < total; ++n) {
        const std::size_t s = n / syllable;
        const double* vowel = formants[(s * 3) % 5];
        const bool talking = (s % 7) != 5 && (s % 7) != 6; // two pauses per seven
        const double pos = static_cast<double>(n % syllable) / static_cast<double>(syllable);
        const double target = talking ? std::pow(std::sin(kPi * pos), 0.6) : 0.0;
        envelope += (target - envelope) * 0.002;
        const double f0 =
            115.0 * (1.0 + 0.08 * std::sin(2.0 * kPi * 0.7 * n / kFs)) * (1.0 + 0.01 * uniform());
        phase += f0 / kFs;
        double excitation = 0.0;
        if (phase >= 1.0) {
            phase -= 1.0;
            excitation = 1.0;
        }
        const double x = excitation * envelope + 0.002 * uniform();
        double acc = 0.0;
        for (int i = 0; i < 3; ++i) {
            const double r = std::exp(-kPi * bandwidths[i] / kFs);
            const double a1 = 2.0 * r * std::cos(2.0 * kPi * vowel[i] / kFs);
            const double y = x + a1 * state[i][0] - r * r * state[i][1];
            state[i][1] = state[i][0];
            state[i][0] = y;
            acc += y * weights[i];
        }
        out[n] = acc;
        peak = std::max(peak, std::fabs(acc));
    }
    std::vector<float> result(total);
    for (std::size_t n = 0; n < total; ++n) {
        result[n] = static_cast<float>(0.5 * out[n] / std::max(peak, 1e-12)); // -6 dBFS peak
    }
    return result;
}

const char* const kKnownKeys[] = {
    "low",
    "high",
    "gain",
    "hl2",
    "hl2-alc",
    "eq-hpf",
    "eq-p1-freq",
    "eq-p1-gain",
    "eq-p1-q",
    "eq-p2-freq",
    "eq-p2-gain",
    "eq-p2-q",
    "eq-p3-freq",
    "eq-p3-gain",
    "eq-p3-q",
    "eq-p4-freq",
    "eq-p4-gain",
    "eq-p4-q",
    "comp-threshold",
    "comp-ratio",
    "comp-attack",
    "comp-release",
    "comp-knee",
    "comp-makeup",
    "comp-drive",
    "comp-phase",
    "comp-ceiling",
    "tube-drive",
    "tube-model",
    "tube-tone",
    "tube-bias",
    "tube-drywet",
    "tube-output",
    "exciter-air",
    "exciter-mix",
    "exciter-low-mix",
    "exciter-mode",
    "lim-ceiling",
    "lim-lookahead",
    "trim",
};

struct Settings {
    std::map<std::string, double> values;
    bool has(const char* key) const { return values.count(key) != 0; }
    bool any(const char* prefix) const
    {
        const std::size_t n = std::strlen(prefix);
        for (const auto& [key, value] : values) {
            if (key.compare(0, n, prefix) == 0) {
                return true;
            }
        }
        return false;
    }
    float get(const char* key, double fallback) const
    {
        const auto it = values.find(key);
        return static_cast<float>(it == values.end() ? fallback : it->second);
    }
};

struct Chain {
    ClientEq eq;
    ClientComp comp;
    ClientTube tube;
    ClientPudu pudu;
    ClientFinalLimiter limiter;

    void configure(const Settings& s, bool processed)
    {
        eq.setEnabled(false);
        comp.setEnabled(false);
        tube.setEnabled(false);
        pudu.setEnabled(false);
        limiter.setEnabled(false);
        limiter.setDcBlockEnabled(false);
        if (!processed) {
            return;
        }

        if (s.any("eq-")) {
            int bands = 0;
            if (s.has("eq-hpf")) {
                ClientEq::BandParams hpf;
                hpf.type = ClientEq::FilterType::HighPass;
                hpf.freqHz = s.get("eq-hpf", 80);
                hpf.slopeDbPerOct = 24;
                eq.setBand(bands++, hpf);
            }
            for (int i = 1; i <= 4; ++i) {
                const std::string prefix = "eq-p" + std::to_string(i) + "-";
                if (!s.any(prefix.c_str())) {
                    continue;
                }
                ClientEq::BandParams peak;
                peak.type = ClientEq::FilterType::Peak;
                peak.freqHz = s.get((prefix + "freq").c_str(), 1000);
                peak.gainDb = s.get((prefix + "gain").c_str(), 0);
                peak.q = s.get((prefix + "q").c_str(), 1);
                eq.setBand(bands++, peak);
            }
            eq.setActiveBandCount(bands);
            eq.setEnabled(bands > 0);
        }
        if (s.any("comp-")) {
            comp.setThresholdDb(s.get("comp-threshold", -20));
            comp.setRatio(s.get("comp-ratio", 4));
            comp.setAttackMs(s.get("comp-attack", 20));
            comp.setReleaseMs(s.get("comp-release", 150));
            comp.setKneeDb(s.get("comp-knee", 6));
            comp.setMakeupDb(s.get("comp-makeup", 0));
            comp.setDriveDb(s.get("comp-drive", 0));
            comp.setPhaseRotatorStages(static_cast<int>(s.get("comp-phase", 0)));
            comp.setLimiterEnabled(s.has("comp-ceiling"));
            comp.setLimiterCeilingDb(s.get("comp-ceiling", -1));
            comp.setEnabled(true);
        }
        if (s.any("tube-")) {
            const int model = static_cast<int>(s.get("tube-model", 0));
            tube.setModel(model == 2       ? ClientTube::Model::C
                              : model == 1 ? ClientTube::Model::B
                                           : ClientTube::Model::A);
            tube.setDriveDb(s.get("tube-drive", 6));
            tube.setTone(s.get("tube-tone", 0));
            tube.setBiasAmount(s.get("tube-bias", 0));
            tube.setDryWet(s.get("tube-drywet", 0.4));
            tube.setOutputGainDb(s.get("tube-output", 0));
            tube.setEnabled(true);
        }
        if (s.any("exciter-")) {
            pudu.setMode(s.get("exciter-mode", 0) > 0.5 ? ClientPudu::Mode::Behringer
                                                        : ClientPudu::Mode::Aphex);
            pudu.setDooHarmonicsDb(s.get("exciter-air", 6));
            pudu.setDooMix(s.get("exciter-mix", 0.2));
            pudu.setPooMix(s.get("exciter-low-mix", 0));
            pudu.setEnabled(true);
        }
        limiter.setEnabled(s.has("lim-ceiling") || s.has("lim-lookahead"));
        limiter.setCeilingDb(s.get("lim-ceiling", ClientFinalLimiter::kDefaultCeilingDb));
        limiter.setLookaheadMs(s.get("lim-lookahead", ClientFinalLimiter::kDefaultLookaheadMs));
        limiter.setOutputTrimDb(s.get("trim", 0));
    }
};

struct Run {
    std::vector<float> transport; // left channel at 24 kHz, before int16
    int latencyFrames48{0};
    double postChainPeak{0.0};        // 48 kHz, after the chain, before Final Output
    double postLimiterPeakDb{-200.0}; // 48 kHz, after Final Output
    std::size_t int16Clips{0};        // transport samples at or over full scale
    double meanBlockUs{0.0};          // processFloat48 per 10 ms block
    double maxBlockUs{0.0};
};

Run runChain(const std::vector<float>& input, const Settings& s, bool processed)
{
    auto chain = std::make_unique<Chain>();
    chain->configure(s, processed);

    TxVoiceProcessor processor;
    TxVoiceProcessor::Processors processors;
    processors.eq = &chain->eq;
    processors.comp = &chain->comp;
    processors.tube = &chain->tube;
    processors.pudu = &chain->pudu;
    processors.finalLimiter = &chain->limiter;
    processor.setProcessors(processors);
    using Stage = TxVoiceProcessor::Stage;
    const std::vector<Stage> stages{Stage::Tube, Stage::Eq, Stage::Comp, Stage::Enh};
    uint64_t order = 0;
    if (!TxChainLayout::pack(stages, order)) {
        std::fprintf(stderr, "the stage order does not fit the packed chain\n");
        return {};
    }
    processor.setStageOrder(order);

    constexpr int kBlock = 480; // 10 ms at 48 kHz
    Run run;
    if (!processor.prepare(TxVoiceProcessor::kDspRate, kBlock)) {
        return run;
    }
    processor.setMeasurementCaptureEnabled(true);
    run.latencyFrames48 = processor.latencyFrames();

    std::vector<float> stereo(kBlock * 2);
    QByteArray transport;
    double totalUs = 0.0;
    std::size_t blocks = 0;
    for (std::size_t at = 0; at + kBlock <= input.size(); at += kBlock) {
        for (int i = 0; i < kBlock; ++i) {
            stereo[i * 2] = stereo[i * 2 + 1] = input[at + static_cast<std::size_t>(i)];
        }
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = processor.processFloat48(stereo.data(), kBlock, transport);
        const double us =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0)
                .count();
        totalUs += us;
        run.maxBlockUs = std::max(run.maxBlockUs, us);
        ++blocks;
        if (!ok) {
            continue;
        }

        const QByteArray& strip = processor.postChannelStripFloat48Stereo();
        const auto* post = reinterpret_cast<const float*>(strip.constData());
        for (qsizetype i = 0; i < strip.size() / static_cast<qsizetype>(sizeof(float)); ++i) {
            run.postChainPeak =
                std::max(run.postChainPeak, static_cast<double>(std::fabs(post[i])));
        }
        run.postLimiterPeakDb =
            std::max(run.postLimiterPeakDb, static_cast<double>(chain->limiter.outputPeakDb()));
        const QByteArray& floats = processor.transportFloat32Stereo();
        const auto* samples = reinterpret_cast<const float*>(floats.constData());
        const int frames = static_cast<int>(floats.size() / (2 * sizeof(float)));
        for (int i = 0; i < frames; ++i) {
            const float v = samples[i * 2];
            if (std::fabs(v) >= 32767.0f / 32768.0f) {
                ++run.int16Clips;
            }
            run.transport.push_back(v);
        }
    }
    run.meanBlockUs = blocks ? totalUs / static_cast<double>(blocks) : 0.0;
    return run;
}

#ifndef TX_PAPR_HARNESS_NO_HL2
// The transport audio through the real HL2 modulator, as Hl2Backend hands it
// over: int16 at 24 kHz, as mono float. The modulator is paced in real time
// (its channel is non-blocking; feeding it faster starves it), so this takes
// as long as the audio lasts. Returns IQ at 48 kHz with the channel's 48.7 ms
// priming (mute ramp and fill) dropped.
std::vector<std::complex<float>> runHl2(const std::vector<float>& transport, const Settings& s,
                                        unsigned long long* faults)
{
    hl2::Hl2TxDsp tx;
    TxTestAuthority authority;
    hl2::Hl2TxDsp::Config cfg;
    cfg.filterLowHz = std::fabs(s.get("low", 300));
    cfg.filterHighHz = std::fabs(s.get("high", 2700));
    cfg.alcEnabled = s.get("hl2-alc", 0) > 0.5;
    std::string error;
    if (!tx.configure(cfg, &error)) {
        std::fprintf(stderr, "HL2 modulator: %s\n", error.c_str());
        return {};
    }
    tx.setMicGain(1.0);

    std::vector<std::complex<float>> iq;
    QObject::connect(&tx, &hl2::Hl2TxDsp::iqReady, &tx,
                     [&](const std::vector<std::complex<float>>& block) {
                         iq.insert(iq.end(), block.begin(), block.end());
                     });
    constexpr std::size_t kChunk = 240; // 10 ms at 24 kHz
    for (std::size_t at = 0; at < transport.size(); at += kChunk) {
        const std::size_t n = std::min(kChunk, transport.size() - at);
        std::vector<float> chunk(n);
        for (std::size_t i = 0; i < n; ++i) {
            const float v = std::clamp(transport[at + i], -1.0f, 32767.0f / 32768.0f);
            chunk[i] = std::round(v * 32768.0f) / 32768.0f; // the int16 hop
        }
        tx.processAudioBlock(chunk, TxAudioSource::Microphone, authority.context);
        std::this_thread::sleep_for(
            std::chrono::duration<double>(static_cast<double>(n) / cfg.inputSampleRateHz));
    }
    *faults = tx.modulatorFaultBlocks();
    constexpr std::size_t kPriming = 2336;
    if (iq.size() <= kPriming) {
        return {};
    }
    return {iq.begin() + static_cast<std::ptrdiff_t>(kPriming), iq.end()};
}
#endif

double dbfs(double linear)
{
    return 20.0 * std::log10(std::max(linear, 1e-9));
}

void printRow(const char* label, const SsbEnvelopeReport& r, bool iq)
{
    if (iq) {
        std::printf("%-26s %8s %8.3f %8.2f %8.2f %8.2f %8s %8s\n", label, "-", dbfs(r.envelopePeak),
                    dbfs(r.activeEnvelopeRms), r.activePaprDb, r.activeP999PaprDb, "-", "-");
    } else {
        std::printf("%-26s %8.2f %8.3f %8.2f %8.2f %8.2f %8.2f %8.1f\n", label, dbfs(r.audioPeak),
                    dbfs(r.envelopePeak), dbfs(r.activeEnvelopeRms), r.activePaprDb,
                    r.activeP999PaprDb, r.envelopeOverAudioPeakDb, r.outOfBandDbc);
    }
}

void printGain(const SsbEnvelopeReport& base, const SsbEnvelopeReport& proc, bool iq)
{
    std::printf("%-26s talk-power gain %+.2f dB active, %+.2f dB p99.9, %+.2f dB whole signal", "",
                base.activePaprDb - proc.activePaprDb,
                base.activeP999PaprDb - proc.activeP999PaprDb, base.paprDb - proc.paprDb);
    // OOB as a change: the raw figure mostly measures what the recording
    // already carries outside the passband (a voice's fundamental sits under
    // 300 Hz), not what the processing added.
    if (!iq) {
        std::printf("; OOB change %+.1f dB", proc.outOfBandDbc - base.outOfBandDbc);
    }
    std::printf("\n\n");
}

std::vector<std::uint8_t> shifted(const std::vector<std::uint8_t>& mask, long blocks)
{
    std::vector<std::uint8_t> out(mask.size(), 0);
    for (std::size_t b = 0; b < mask.size(); ++b) {
        const long to = static_cast<long>(b) + blocks;
        if (to >= 0 && to < static_cast<long>(out.size())) {
            out[static_cast<std::size_t>(to)] = mask[b];
        }
    }
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <input-48k.wav | synthetic> [--key value ...]\n", argv[0]);
        return 2;
    }
    Settings settings;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (std::strncmp(argv[i], "--", 2) != 0) {
            std::fprintf(stderr, "unexpected argument %s\n", argv[i]);
            return 2;
        }
        const std::string key = argv[i] + 2;
        bool known = false;
        for (const char* k : kKnownKeys) {
            known = known || key == k;
        }
        char* end = nullptr;
        const double value = std::strtod(argv[i + 1], &end);
        if (!known || end == argv[i + 1] || *end != '\0') {
            std::fprintf(stderr, "%s %s: %s\n", argv[i], argv[i + 1],
                         known ? "not a number" : "unknown flag (see the header of this file)");
            return 2;
        }
        settings.values[key] = value;
    }
    if (argc % 2 != 0) {
        std::fprintf(stderr, "%s has no value\n", argv[argc - 1]);
        return 2;
    }
#ifdef TX_PAPR_HARNESS_NO_HL2
    if (settings.get("hl2", 0) > 0.5) {
        std::fprintf(stderr, "--hl2: this build has no HL2 modulator (TX_PAPR_HARNESS_NO_HL2)\n");
        return 2;
    }
#endif

    std::vector<float> input;
    std::string error;
    if (std::strcmp(argv[1], "synthetic") == 0) {
        input = synthesizeVoice(12.0);
    } else if (!readWav48(argv[1], input, error)) {
        std::fprintf(stderr, "%s: %s\n", argv[1], error.c_str());
        return 2;
    }
    const double gain = std::pow(10.0, settings.get("gain", 0) / 20.0);
    for (auto& v : input) {
        v = static_cast<float>(v * gain);
    }

    const SsbPassband band{settings.get("low", 300), settings.get("high", 2700)};
    const Run baseline = runChain(input, settings, false);
    const Run processed = runChain(input, settings, true);
    if (baseline.transport.empty() || processed.transport.empty()) {
        std::fprintf(stderr, "the voice processor produced no output\n");
        return 1;
    }

    std::printf("input %s, %.2f s, passband %.0f..%.0f Hz, transport 24 kHz int16\n", argv[1],
                static_cast<double>(input.size()) / 48000.0, band.lowHz, band.highHz);
    std::printf("settings:");
    for (const auto& [key, value] : settings.values) {
        std::printf(" --%s %g", key.c_str(), value);
    }
    std::printf("%s\n", settings.values.empty() ? " (none: both runs bypassed)" : "");
    std::printf("added latency: baseline %.1f ms, processed %.1f ms\n",
                baseline.latencyFrames48 / 48.0, processed.latencyFrames48 / 48.0);
    for (const Run* r : {&baseline, &processed}) {
        double transportPeak = 0.0;
        for (float v : r->transport) {
            transportPeak = std::max(transportPeak, static_cast<double>(std::fabs(v)));
        }
        std::printf("%-9s peaks: after chain %6.2f dBFS, after Final Output %6.2f dBFS, "
                    "transport %6.2f dBFS, int16 clips %zu\n",
                    r == &baseline ? "baseline" : "processed", dbfs(r->postChainPeak),
                    r->postLimiterPeakDb, dbfs(transportPeak), r->int16Clips);
        std::printf("%-9s CPU: %.1f us mean, %.1f us worst per 10 ms block (%.2f%% / %.2f%% of "
                    "real time)\n",
                    "", r->meanBlockUs, r->maxBlockUs, r->meanBlockUs / 100.0,
                    r->maxBlockUs / 100.0);
    }
    std::printf("\n%-26s %8s %8s %8s %8s %8s %8s %8s\n", "run / radio", "audio", "env pk",
                "act rms", "PAPR", "p99.9", "env/aud", "OOB");
    std::printf("%-26s %8s %8s %8s %8s %8s %8s %8s\n", "", "dBFS", "dBFS", "dBFS", "dB", "dB", "dB",
                "dBc");

    // Both runs are scored over the bypassed run's active blocks, shifted by
    // any latency the processing adds (20 ms blocks).
    const long shiftBlocks =
        std::lround((processed.latencyFrames48 - baseline.latencyFrames48) / 48.0 / 20.0);
    for (const auto phase : {SsbFilterPhase::Linear, SsbFilterPhase::Minimum}) {
        const char* name =
            phase == SsbFilterPhase::Linear ? "linear filter" : "minimum-phase filter";
        std::vector<std::uint8_t> mask;
        const auto base = measureSsbEnvelope(baseline.transport.data(), baseline.transport.size(),
                                             24000.0, band, phase, 1023, nullptr, &mask);
        const auto maskShifted = shifted(mask, shiftBlocks);
        const auto proc = measureSsbEnvelope(processed.transport.data(), processed.transport.size(),
                                             24000.0, band, phase, 1023, &maskShifted);
        printRow((std::string("baseline / ") + name).c_str(), base, false);
        printRow((std::string("processed / ") + name).c_str(), proc, false);
        printGain(base, proc, false);
    }

#ifndef TX_PAPR_HARNESS_NO_HL2
    if (settings.get("hl2", 0) > 0.5) {
        unsigned long long baseFaults = 0, procFaults = 0;
        std::printf("HL2 modulator (real time, %.0f s per run, ALC %s)...\n",
                    static_cast<double>(baseline.transport.size()) / 24000.0,
                    settings.get("hl2-alc", 0) > 0.5 ? "on" : "off");
        std::fflush(stdout);
        const auto baseIq = runHl2(baseline.transport, settings, &baseFaults);
        const auto procIq = runHl2(processed.transport, settings, &procFaults);
        if (baseIq.empty() || procIq.empty()) {
            std::fprintf(stderr, "the HL2 modulator produced no IQ\n");
            return 1;
        }
        std::vector<std::uint8_t> mask;
        const auto base = measureIqEnvelope(baseIq.data(), baseIq.size(), 48000.0, nullptr, &mask);
        const auto maskShifted = shifted(mask, shiftBlocks);
        const auto proc = measureIqEnvelope(procIq.data(), procIq.size(), 48000.0, &maskShifted);
        printRow("baseline / HL2 modulator", base, true);
        printRow("processed / HL2 modulator", proc, true);
        printGain(base, proc, true);
        if (baseFaults || procFaults) {
            std::printf("WARNING: modulator fault blocks %llu / %llu (starved or late); "
                        "rerun on an idle machine\n",
                        baseFaults, procFaults);
        }
    }
#endif
    return 0;
}
