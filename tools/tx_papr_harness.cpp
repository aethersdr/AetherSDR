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
//       [--lim-ceiling -1] [--trim 0] [--gain 0]
//
// --comp-* turns the Compressor stage on, --comp-ceiling its own limiter,
// --lim-ceiling the Final Output LIM. --gain is applied to the recording
// before both runs, to place a quiet recording at a realistic mic level.
// The input must be 48 kHz PCM16 or float32 WAV, mono or stereo (left used).

#include "core/ClientComp.h"
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
    std::vector<float> transport;  // left channel at 24 kHz
    int latencyFrames48{0};
};

Run runChain(const std::vector<float>& input, const Settings& s, bool processed)
{
    ClientComp comp;
    ClientFinalLimiter limiter;
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
        limiter.setEnabled(s.has("lim-ceiling"));
        limiter.setCeilingDb(static_cast<float>(s.get("lim-ceiling", -1.0)));
        limiter.setOutputTrimDb(static_cast<float>(s.get("trim", 0.0)));
    }

    TxVoiceProcessor processor;
    TxVoiceProcessor::Processors processors;
    processors.comp = &comp;
    processors.finalLimiter = &limiter;
    processor.setProcessors(processors);
    processor.setStageOrder(static_cast<uint64_t>(TxVoiceProcessor::Stage::Comp));
    constexpr int kBlock = 480;  // 10 ms at 48 kHz
    Run run;
    if (!processor.prepare(TxVoiceProcessor::kDspRate, kBlock)) return run;
    run.latencyFrames48 = processor.latencyFrames();

    std::vector<float> stereo(kBlock * 2);
    QByteArray transport;
    for (std::size_t at = 0; at + kBlock <= input.size(); at += kBlock) {
        for (int i = 0; i < kBlock; ++i) {
            stereo[i * 2] = stereo[i * 2 + 1] = input[at + static_cast<std::size_t>(i)];
        }
        if (!processor.processFloat48(stereo.data(), kBlock, transport)) continue;
        const auto* samples = reinterpret_cast<const int16_t*>(transport.constData());
        const int frames = static_cast<int>(transport.size() / (2 * sizeof(int16_t)));
        for (int i = 0; i < frames; ++i)
            run.transport.push_back(static_cast<float>(samples[i * 2]) / 32768.0f);
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
    if (!readWav48(argv[1], input, error)) {
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
    std::printf("added latency: baseline %.1f ms, processed %.1f ms\n\n",
                baseline.latencyFrames48 / 48.0, processed.latencyFrames48 / 48.0);
    std::printf("%-22s %8s %8s %8s %8s %8s %8s %8s\n", "run / radio filter", "audio", "env pk",
                "act rms", "PAPR", "p99.9", "env/aud", "OOB");
    std::printf("%-22s %8s %8s %8s %8s %8s %8s %8s\n", "", "dBFS", "dBFS", "dBFS", "dB", "dB",
                "dB", "dBc");

    for (const auto phase : {SsbFilterPhase::Linear, SsbFilterPhase::Minimum}) {
        const char* phaseName = phase == SsbFilterPhase::Linear ? "linear" : "minimum";
        SsbEnvelopeReport reports[2];
        const Run* runs[2] = {&baseline, &processed};
        const char* names[2] = {"baseline", "processed"};
        for (int r = 0; r < 2; ++r) {
            reports[r] = measureSsbEnvelope(runs[r]->transport.data(), runs[r]->transport.size(),
                                            24000.0, band, phase);
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
        std::printf("%-22s talk-power gain %+.2f dB (p99.9 basis %+.2f dB), OOB change %+.1f dB\n\n",
                    "", reports[0].activePaprDb - reports[1].activePaprDb,
                    reports[0].activeP999PaprDb - reports[1].activeP999PaprDb,
                    reports[1].outOfBandDbc - reports[0].outOfBandDbc);
    }
    return 0;
}
