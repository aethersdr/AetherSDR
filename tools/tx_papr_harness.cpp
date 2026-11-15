// Offline SSB peak-to-average harness (RFC #4836, Stage 0).
//
// Runs a local recording through the real TX voice processor twice -- every
// stage off, then with the dynamics settings given on the command line --
// and measures the 24 kHz transport output the radio would receive with the
// SSB envelope meter. Nothing is keyed and no radio is opened.
//
//   tx_papr_harness voice-48k.wav [--low 300] [--high 2700]
//       [--comp-threshold -20] [--comp-ratio 4] [--comp-attack 20]
//       [--comp-release 150] [--comp-knee 6] [--comp-makeup 5]
//       [--comp-drive 0] [--comp-phase 0] [--comp-ceiling -1]
//       [--lim-ceiling -1] [--trim 0] [--gain 0] [--eq-hpf 200]
//
// --comp-* turns the Compressor stage on, --comp-ceiling its own limiter,
// --lim-ceiling the Final Output LIM, --eq-hpf a 24 dB/oct EQ high-pass ahead
// of the compressor (processed run only). --gain is applied to the recording
// before both runs, to place a quiet recording at a realistic mic level.
// The input must be 48 kHz PCM16 or float32 WAV, mono or stereo (left used),
// or the word `synthetic` for a deterministic 12 s voiced-speech stand-in
// (jittered glottal pulses through three formant resonators, with pauses), so
// a run is reproducible without a recording.

#include "core/ClientComp.h"
#include "core/ClientEq.h"
#include "core/ClientFinalLimiter.h"
#include "core/TxVoiceProcessor.h"
#include "core/dsp/SsbEnvelopeMeter.h"

#include <QByteArray>
#include <QFile>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace AetherSDR;

namespace {

bool readWav48(const char* path, std::vector<float>& mono, std::string& error)
{
    QFile file(QString::fromLocal8Bit(path));
    if (!file.open(QIODevice::ReadOnly)) { error = "cannot open input"; return false; }
    const QByteArray bytes = file.readAll();
    if (bytes.size() < 12 || std::memcmp(bytes.constData(), "RIFF", 4) != 0
        || std::memcmp(bytes.constData() + 8, "WAVE", 4) != 0) {
        error = "not a RIFF/WAVE file";
        return false;
    }
    auto u16 = [&](qsizetype at) {
        return static_cast<uint16_t>(static_cast<uint8_t>(bytes[at])
                                     | static_cast<uint8_t>(bytes[at + 1]) << 8);
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
            if (format == 0xFFFE && size >= 40) format = u16(at + 32);  // extensible
        } else if (std::memcmp(bytes.constData() + at, "data", 4) == 0) {
            dataAt = at + 8;
            dataSize = static_cast<uint32_t>(std::min<qsizetype>(size, bytes.size() - dataAt));
        }
        at += 8 + size + (size & 1);
    }
    if (dataAt < 0 || channels == 0) { error = "no fmt or data chunk"; return false; }
    if (rate != 48000) { error = "sample rate is not 48000 Hz (resample first)"; return false; }
    const bool pcm16 = format == 1 && bits == 16;
    const bool float32 = format == 3 && bits == 32;
    if (!pcm16 && !float32) { error = "only PCM16 and float32 are supported"; return false; }

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
    const double formants[5][3] = {{730, 1090, 2440}, {270, 2290, 3010}, {530, 1840, 2480},
                                   {300, 870, 2240}, {660, 1720, 2410}};
    const double bandwidths[3] = {80, 100, 140};
    const double weights[3] = {1.0, 0.6, 0.35};
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> uniform(-0.5, 0.5);

    const std::size_t total = static_cast<std::size_t>(seconds * kFs);
    const std::size_t syllable = static_cast<std::size_t>(0.22 * kFs);
    std::vector<double> out(total);
    double state[3][2] = {};
    double phase = 0.0, envelope = 0.0, peak = 0.0;
    for (std::size_t n = 0; n < total; ++n) {
        const std::size_t s = n / syllable;
        const double* vowel = formants[(s * 3) % 5];
        const bool talking = (s % 7) != 5 && (s % 7) != 6;  // two pauses per seven
        const double pos = static_cast<double>(n % syllable) / static_cast<double>(syllable);
        const double target = talking ? std::pow(std::sin(kPi * pos), 0.6) : 0.0;
        envelope += (target - envelope) * 0.002;
        const double f0 = 115.0 * (1.0 + 0.08 * std::sin(2.0 * kPi * 0.7 * n / kFs))
                        * (1.0 + 0.01 * uniform(rng));
        phase += f0 / kFs;
        double excitation = 0.0;
        if (phase >= 1.0) { phase -= 1.0; excitation = 1.0; }
        const double x = excitation * envelope + 0.002 * uniform(rng);
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
    for (std::size_t n = 0; n < total; ++n)
        result[n] = static_cast<float>(0.5 * out[n] / std::max(peak, 1e-12));  // -6 dBFS peak
    return result;
}

struct Settings {
    std::map<std::string, double> values;
    bool has(const char* key) const { return values.count(key) != 0; }
    double get(const char* key, double fallback) const
    {
        const auto it = values.find(key);
        return it == values.end() ? fallback : it->second;
    }
};

struct Run {
    std::vector<float> transport;  // left channel at 24 kHz, before int16
    int latencyFrames48{0};
    double postChainPeak{0.0};     // 48 kHz, after the chain, before Final Output
    double postLimiterPeakDb{-200.0};  // 48 kHz, after Final Output
    std::size_t int16Clips{0};     // transport samples at or over full scale
};

Run runChain(const std::vector<float>& input, const Settings& s, bool processed)
{
    ClientEq eq;
    ClientComp comp;
    ClientFinalLimiter limiter;
    eq.setEnabled(false);
    comp.setEnabled(false);
    limiter.setEnabled(false);
    limiter.setDcBlockEnabled(false);
    if (processed) {
        const bool compOn = s.has("comp-threshold") || s.has("comp-ratio") || s.has("comp-makeup")
            || s.has("comp-drive") || s.has("comp-phase") || s.has("comp-ceiling")
            || s.has("comp-attack") || s.has("comp-release") || s.has("comp-knee");
        comp.setEnabled(compOn);
        comp.setThresholdDb(static_cast<float>(s.get("comp-threshold", -20.0)));
        comp.setRatio(static_cast<float>(s.get("comp-ratio", 4.0)));
        comp.setAttackMs(static_cast<float>(s.get("comp-attack", 20.0)));
        comp.setReleaseMs(static_cast<float>(s.get("comp-release", 150.0)));
        comp.setKneeDb(static_cast<float>(s.get("comp-knee", 6.0)));
        comp.setMakeupDb(static_cast<float>(s.get("comp-makeup", 0.0)));
        comp.setDriveDb(static_cast<float>(s.get("comp-drive", 0.0)));
        comp.setPhaseRotatorStages(static_cast<int>(s.get("comp-phase", 0.0)));
        comp.setLimiterEnabled(s.has("comp-ceiling"));
        comp.setLimiterCeilingDb(static_cast<float>(s.get("comp-ceiling", -1.0)));
        if (s.has("eq-hpf")) {
            ClientEq::BandParams hpf;
            hpf.type = ClientEq::FilterType::HighPass;
            hpf.freqHz = static_cast<float>(s.get("eq-hpf", 200.0));
            hpf.slopeDbPerOct = 24;
            eq.setBand(0, hpf);
            eq.setActiveBandCount(1);
            eq.setEnabled(true);
        }
        limiter.setEnabled(s.has("lim-ceiling"));
        limiter.setCeilingDb(static_cast<float>(s.get("lim-ceiling", -1.0)));
        limiter.setOutputTrimDb(static_cast<float>(s.get("trim", 0.0)));
    }

    TxVoiceProcessor processor;
    TxVoiceProcessor::Processors processors;
    processors.eq = &eq;
    processors.comp = &comp;
    processors.finalLimiter = &limiter;
    processor.setProcessors(processors);
    // EQ in slot 0, Compressor in slot 1: the shipped default order.
    processor.setStageOrder(static_cast<uint64_t>(TxVoiceProcessor::Stage::Eq)
                            | static_cast<uint64_t>(TxVoiceProcessor::Stage::Comp) << 8);
    constexpr int kBlock = 480;  // 10 ms at 48 kHz
    Run run;
    if (!processor.prepare(TxVoiceProcessor::kDspRate, kBlock)) return run;
    processor.setMeasurementCaptureEnabled(true);
    run.latencyFrames48 = processor.latencyFrames();

    std::vector<float> stereo(kBlock * 2);
    QByteArray transport;
    for (std::size_t at = 0; at + kBlock <= input.size(); at += kBlock) {
        for (int i = 0; i < kBlock; ++i) {
            stereo[i * 2] = stereo[i * 2 + 1] = input[at + static_cast<std::size_t>(i)];
        }
        if (!processor.processFloat48(stereo.data(), kBlock, transport)) continue;
        const QByteArray& strip = processor.postChannelStripFloat48Stereo();
        const auto* chain = reinterpret_cast<const float*>(strip.constData());
        for (qsizetype i = 0; i < strip.size() / static_cast<qsizetype>(sizeof(float)); ++i)
            run.postChainPeak = std::max(run.postChainPeak, static_cast<double>(std::fabs(chain[i])));
        run.postLimiterPeakDb = std::max(run.postLimiterPeakDb,
                                         static_cast<double>(limiter.outputPeakDb()));
        const QByteArray& floats = processor.transportFloat32Stereo();
        const auto* samples = reinterpret_cast<const float*>(floats.constData());
        const int frames = static_cast<int>(floats.size() / (2 * sizeof(float)));
        for (int i = 0; i < frames; ++i) {
            const float v = samples[i * 2];
            if (std::fabs(v) >= 32767.0f / 32768.0f) ++run.int16Clips;
            run.transport.push_back(v);
        }
    }
    return run;
}

double dbfs(double linear) { return 20.0 * std::log10(std::max(linear, 1e-9)); }

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s input-48k.wav [--key value ...]\n", argv[0]);
        return 2;
    }
    Settings settings;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (std::strncmp(argv[i], "--", 2) != 0) {
            std::fprintf(stderr, "unexpected argument %s\n", argv[i]);
            return 2;
        }
        settings.values[argv[i] + 2] = std::atof(argv[i + 1]);
    }

    std::vector<float> input;
    std::string error;
    if (std::strcmp(argv[1], "synthetic") == 0) {
        input = synthesizeVoice(12.0);
    } else if (!readWav48(argv[1], input, error)) {
        std::fprintf(stderr, "%s: %s\n", argv[1], error.c_str());
        return 2;
    }
    const double gain = std::pow(10.0, settings.get("gain", 0.0) / 20.0);
    for (auto& v : input) v = static_cast<float>(v * gain);

    const SsbPassband band{settings.get("low", 300.0), settings.get("high", 2700.0)};
    const Run baseline = runChain(input, settings, false);
    const Run processed = runChain(input, settings, true);
    if (baseline.transport.empty() || processed.transport.empty()) {
        std::fprintf(stderr, "the voice processor produced no output\n");
        return 1;
    }

    std::printf("input %.2f s, passband %.0f..%.0f Hz, transport 24 kHz int16\n",
                static_cast<double>(input.size()) / 48000.0, band.lowHz, band.highHz);
    std::printf("added latency: baseline %.1f ms, processed %.1f ms\n",
                baseline.latencyFrames48 / 48.0, processed.latencyFrames48 / 48.0);
    for (const Run* r : {&baseline, &processed}) {
        double transportPeak = 0.0;
        for (float v : r->transport) transportPeak = std::max(transportPeak, static_cast<double>(std::fabs(v)));
        std::printf("%-9s peaks: after chain %6.2f dBFS, after Final Output %6.2f dBFS, "
                    "transport %6.2f dBFS, int16 clips %zu\n",
                    r == &baseline ? "baseline" : "processed", dbfs(r->postChainPeak),
                    r->postLimiterPeakDb, dbfs(transportPeak), r->int16Clips);
    }
    std::printf("\n");
    std::printf("%-22s %8s %8s %8s %8s %8s %8s %8s\n", "run / radio filter", "audio", "env pk",
                "act rms", "PAPR", "p99.9", "env/aud", "OOB");
    std::printf("%-22s %8s %8s %8s %8s %8s %8s %8s\n", "", "dBFS", "dBFS", "dBFS", "dB", "dB",
                "dB", "dBc");

    for (const auto phase : {SsbFilterPhase::Linear, SsbFilterPhase::Minimum}) {
        const char* phaseName = phase == SsbFilterPhase::Linear ? "linear" : "minimum";
        SsbEnvelopeReport reports[2];
        const Run* runs[2] = {&baseline, &processed};
        const char* names[2] = {"baseline", "processed"};
        // Both runs are scored over the bypassed run's active blocks, shifted
        // by any latency the processing adds (20 ms blocks at 24 kHz).
        std::vector<std::uint8_t> baseMask, shiftedMask;
        const long shiftBlocks = std::lround(
            (processed.latencyFrames48 - baseline.latencyFrames48) / 2.0 / 480.0);
        for (int r = 0; r < 2; ++r) {
            if (r == 1) {
                shiftedMask.assign(baseMask.size(), 0);
                for (std::size_t b = 0; b < baseMask.size(); ++b) {
                    const long to = static_cast<long>(b) + shiftBlocks;
                    if (to >= 0 && to < static_cast<long>(shiftedMask.size()))
                        shiftedMask[static_cast<std::size_t>(to)] = baseMask[b];
                }
            }
            reports[r] = measureSsbEnvelope(runs[r]->transport.data(), runs[r]->transport.size(),
                                            24000.0, band, phase, 1023,
                                            r == 0 ? nullptr : &shiftedMask,
                                            r == 0 ? &baseMask : nullptr);
            const auto& rep = reports[r];
            const std::string label = std::string(names[r]) + " / " + phaseName;
            std::printf("%-22s %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f %8.1f\n", label.c_str(),
                        dbfs(rep.audioPeak), dbfs(rep.envelopePeak), dbfs(rep.activeEnvelopeRms),
                        rep.activePaprDb, rep.activeP999PaprDb, rep.envelopeOverAudioPeakDb,
                        rep.outOfBandDbc);
        }
        // Talk-power gain: how much more average power the processed audio
        // carries at the same envelope peak (PEP).
        // OOB is reported as a change: the raw figure mostly measures what the
        // recording already carries below and above the passband (a voice's
        // fundamental sits under 300 Hz), not what the processing added.
        std::printf("%-22s talk-power gain %+.2f dB active, %+.2f dB p99.9, %+.2f dB whole signal;"
                    " OOB change %+.1f dB\n\n",
                    "", reports[0].activePaprDb - reports[1].activePaprDb,
                    reports[0].activeP999PaprDb - reports[1].activeP999PaprDb,
                    reports[0].paprDb - reports[1].paprDb,
                    reports[1].outOfBandDbc - reports[0].outOfBandDbc);
    }
    return 0;
}
