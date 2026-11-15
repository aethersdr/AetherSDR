// Pure continuous TCI RX conversion. No sockets, peers, devices or event loop.
#include "core/TciRxConverter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numbers>
#include <span>
#include <utility>

using namespace AetherSDR;

namespace {
int checks = 0;
int failures = 0;
void check(bool pass, const char* message)
{
    ++checks;
    if (!pass) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

std::span<const float> view(const QVector<float>& samples)
{
    return {samples.constData(), static_cast<std::size_t>(samples.size())};
}

QVector<float> tones(int rate, int frames, PcmLayout layout, bool antiphase = false,
                     double leftHz = 1000.0, double rightHz = 1700.0)
{
    const int channels = layout == PcmLayout::Mono ? 1 : 2;
    QVector<float> samples(frames * channels);
    for (int frame = 0; frame < frames; ++frame) {
        const double time = static_cast<double>(frame) / rate;
        const float left = static_cast<float>(0.4 * std::sin(2 * std::numbers::pi * leftHz * time));
        samples[frame * channels] = left;
        if (channels == 2) {
            samples[frame * channels + 1] = antiphase ? -left
                : static_cast<float>(0.2 * std::sin(2 * std::numbers::pi * rightHz * time));
        }
    }
    return samples;
}

bool append(TciRxConverter& converter, std::span<const float> samples,
            QVector<float>& result, int* calls = nullptr)
{
    return converter.process(samples, [&](QVector<float> output) {
        check(!output.isEmpty() && output.size() % 2 == 0 && output.size() <= 2048,
              "each emitted block owns at most 1024 whole stereo frames");
        if (calls) {
            ++*calls;
        }
        result.append(output);
        return true;
    });
}

// Spectral magnitude is independent of the converter's filter phase/delay.
double magnitude(const QVector<float>& samples, int rate, int channel, double hz)
{
    const qsizetype begin = rate / 2;
    const qsizetype end = samples.size() / 2;
    if (begin >= end) {
        return 0.0;
    }
    double real = 0.0;
    double imaginary = 0.0;
    for (qsizetype frame = begin; frame < end; ++frame) {
        const double phase = 2 * std::numbers::pi * hz * static_cast<double>(frame) / rate;
        real += samples[2 * frame + channel] * std::cos(phase);
        imaginary += samples[2 * frame + channel] * std::sin(phase);
    }
    return 2 * std::hypot(real, imaginary) / static_cast<double>(end - begin);
}

void nativeIsExact()
{
    // Catches native-rate filtering, stereo collapse, clipping and borrowed output.
    for (const int rate : {24000, 48000}) {
        TciRxConverter converter({rate, PcmLayout::Stereo}, rate);
        QVector<float> input{0.0f, -0.0f, 1.25f, -1.5f, 0.123f, -0.456f};
        const QVector<float> expected = input;
        QVector<float> output;
        check(append(converter, view(input), output), "native stereo accepts partial staging block");
        input.fill(0.0f);
        check(output.size() == expected.size()
                  && std::memcmp(output.constData(), expected.constData(),
                                 expected.size() * sizeof(float)) == 0,
              "native stereo is immediately byte-exact and independently owned");
        check(converter.inputFrames() == 3 && converter.outputFrames() == 3
                  && converter.stagedInputFrames() == 0 && converter.groupDelayInputFrames() == 0,
              "native frame accounting and delay count LR pairs");

        TciRxConverter mono({rate, PcmLayout::Mono}, rate);
        output.clear();
        const QVector<float> monoSamples{0.0f, -0.0f, 1.25f, -1.5f};
        const QVector<float> stereoSamples{0.0f, 0.0f, -0.0f, -0.0f, 1.25f, 1.25f, -1.5f, -1.5f};
        check(append(mono, view(monoSamples), output), "native mono accepted");
        check(output.size() == stereoSamples.size()
                  && std::memcmp(output.constData(), stereoSamples.constData(),
                                 stereoSamples.size() * sizeof(float)) == 0,
              "only mono input duplicates to stereo without clipping");
    }
}

void rateMatrixAndPartitioning()
{
    // Catches a hard-coded 24 kHz input, wrong ratio, dropped chunk fragments,
    // channel intermixing and rate conversion dependent on producer chunk size.
    constexpr int kFrames = 49373;
    constexpr std::array<int, 7> kChunks{1, 255, 7, 511, 3, 1024, 31};
    for (const int sourceRate : {24000, 48000}) {
        for (const int outputRate : {8000, 12000, 24000, 44100, 48000}) {
            for (const PcmLayout layout : {PcmLayout::Mono, PcmLayout::Stereo}) {
                std::printf("MATRIX: %d -> %d, %s\n", sourceRate, outputRate,
                            layout == PcmLayout::Mono ? "mono" : "stereo");
                const PcmFormat format{sourceRate, layout};
                const QVector<float> input = tones(sourceRate, kFrames, layout);
                TciRxConverter whole(format, outputRate);
                TciRxConverter pieces(format, outputRate);
                QVector<float> oneOutput;
                QVector<float> chunkOutput;
                check(whole.valid() && pieces.valid(), "supported rate/layout pair accepted");
                check(append(whole, view(input), oneOutput), "whole bounded stream accepted");
                int offset = 0;
                std::size_t chunk = 0;
                while (offset < kFrames) {
                    const int count = std::min(kChunks[chunk++ % kChunks.size()], kFrames - offset);
                    check(append(pieces, view(input).subspan(offset * format.channels(),
                                     count * format.channels()), chunkOutput),
                          "arbitrary input partition accepted");
                    offset += count;
                }
                check(oneOutput == chunkOutput, "whole and variable chunks produce identical samples");
                check(whole.inputFrames() == kFrames && pieces.inputFrames() == kFrames,
                      "accepted input frame counts are independent of chunking");
                const bool native = sourceRate == outputRate;
                const int convertedFrames = native ? kFrames : 49152; // 192 full blocks
                const qint64 expectedFloor = static_cast<qint64>(convertedFrames) * outputRate / sourceRate;
                check(std::abs(oneOutput.size() / 2 - expectedFloor) <= 1,
                      "continuous output duration matches actual rate within one frame rounding");
                check(whole.stagedInputFrames() == (native ? 0 : 221)
                          && pieces.stagedInputFrames() == whole.stagedInputFrames(),
                      "only the incomplete input block is held; no finish or zero padding");
                check(whole.outputFrames() == static_cast<quint64>(oneOutput.size() / 2)
                          && pieces.outputFrames() == whole.outputFrames(),
                      "output counter counts accepted emitted stereo frames");
                check(native ? whole.groupDelayInputFrames() == 0
                             : whole.groupDelayInputFrames() > 0,
                      "unequal-rate continuous filter latency is exposed");
                check(std::abs(magnitude(oneOutput, outputRate, 0, 1000) - 0.4) < 0.015,
                      "left tone amplitude and frequency preserved at actual output rate");
                if (layout == PcmLayout::Mono) {
                    bool identical = true;
                    for (qsizetype frame = 0; frame < oneOutput.size() / 2; ++frame) {
                        identical &= oneOutput[2 * frame] == oneOutput[2 * frame + 1];
                    }
                    check(identical, "mono conversion yields identical left and right");
                } else {
                    check(std::abs(magnitude(oneOutput, outputRate, 1, 1700) - 0.2) < 0.015,
                          "right independent tone amplitude and frequency preserved");
                    check(magnitude(oneOutput, outputRate, 0, 1700) < 0.01
                              && magnitude(oneOutput, outputRate, 1, 1000) < 0.01,
                          "independent channel tones never leak across stereo");
                }
            }
        }
    }
}

void antiphaseAndWideAudio()
{
    // Existing processStereoToStereo() averages LR and turns this into silence.
    for (const int sourceRate : {24000, 48000}) {
        for (const int outputRate : {8000, 12000, 24000, 44100, 48000}) {
            TciRxConverter converter({sourceRate, PcmLayout::Stereo}, outputRate);
            QVector<float> output;
            const QVector<float> input = tones(sourceRate, 49152, PcmLayout::Stereo, true);
            check(append(converter, view(input), output), "antiphase stream accepted");
            bool opposite = true;
            for (qsizetype frame = 0; frame < output.size() / 2; ++frame) {
                opposite &= std::abs(output[2 * frame] + output[2 * frame + 1]) < 1e-6f;
            }
            check(opposite && magnitude(output, outputRate, 0, 1000) > 0.38,
                  "antiphase stereo stays audible with opposite left and right");
        }
    }
    for (const int outputRate : {44100, 48000}) {
        TciRxConverter converter({48000, PcmLayout::Stereo}, outputRate);
        QVector<float> output;
        const QVector<float> input = tones(48000, 49152, PcmLayout::Stereo, false, 15000, 1700);
        check(append(converter, view(input), output), "48 kHz wide-audio stream accepted");
        check(std::abs(magnitude(output, outputRate, 0, 15000) - 0.4) < 0.015,
              "15 kHz audio survives when both input and output Nyquist permit it");
        check(magnitude(output, outputRate, 1, 15000) < 0.01,
              "15 kHz left audio does not leak into right");
    }
}

void discardRetiresEverything()
{
    // Catches resampler history or partial input surviving disconnect/restart.
    for (const int rate : {24000, 48000}) {
        for (const int outputRate : {8000, 12000, 24000, 44100, 48000}) {
            TciRxConverter converter({rate, PcmLayout::Stereo}, outputRate);
            QVector<float> output;
            const QVector<float> input = tones(rate, 8265, PcmLayout::Stereo);
            check(append(converter, view(input), output), "history and partial block primed");
            converter.discard();
            converter.discard();
            check(converter.stagedInputFrames() == 0 && converter.inputFrames() == 0
                      && converter.outputFrames() == 0,
                  "repeated discard retires staging and counters");
            output.clear();
            const QVector<float> silence(32768, 0.0f);
            check(append(converter, view(silence), output), "new silence accepted after discard");
            check(!output.isEmpty() && std::ranges::all_of(output, [](float sample) { return sample == 0.0f; }),
                  "retired filter history cannot leak into replacement stream");
        }
    }
}

void declaredLatencyMatchesWaveform()
{
    // Catches a delay getter that describes only no-output warmup rather than
    // the acoustic filter delay, or samples assigned to the wrong clock.
    for (const int sourceRate : {24000, 48000}) {
        for (const int outputRate : {8000, 12000, 24000, 44100, 48000}) {
            TciRxConverter converter({sourceRate, PcmLayout::Stereo}, outputRate);
            QVector<float> impulse(49152 * 2, 0.0f);
            impulse[0] = 1.0f;
            impulse[1] = 0.5f;
            QVector<float> output;
            check(append(converter, view(impulse).first(255 * 2), output),
                  "initial 255 input frames accepted");
            check(sourceRate == outputRate ? output.size() == 510 : output.isEmpty(),
                  "batching waits only for a complete block and native output stays immediate");
            check(append(converter, view(impulse).subspan(255 * 2), output),
                  "continuous impulse completes without a finite tail operation");
            qsizetype peakFrame = 0;
            float peak = 0.0f;
            for (qsizetype frame = 0; frame < output.size() / 2; ++frame) {
                if (std::abs(output[2 * frame]) > peak) {
                    peak = std::abs(output[2 * frame]);
                    peakFrame = frame;
                }
            }
            const double expected = static_cast<double>(converter.groupDelayInputFrames())
                * outputRate / sourceRate;
            check(peak > 0.1f && std::abs(static_cast<double>(peakFrame) - expected) <= 1.0,
                  "measured impulse peak agrees with declared filter delay within one output frame");
        }
    }
}

void boundsAndRejection()
{
    // Catches malformed or poisoned data reaching a sink; invalid calls discard
    // old buffered audio instead of making it available to a later stream.
    for (const PcmFormat format : {PcmFormat{0, PcmLayout::Stereo},
                                  PcmFormat{44100, PcmLayout::Stereo},
                                  PcmFormat{24000, static_cast<PcmLayout>(9)}}) {
        TciRxConverter converter(format, 48000);
        int calls = 0;
        const QVector<float> samples{0.1f, 0.2f};
        check(!converter.valid() && !converter.process(view(samples), [&](QVector<float>) { ++calls; return true; })
                  && calls == 0,
              "invalid producer format is refused without constructing an unsafe filter");
    }
    for (const int rate : {0, -1, 16000, 96000}) {
        TciRxConverter converter({}, rate);
        check(!converter.valid(), "unsupported TCI output rate refused");
    }
    TciRxConverter converter({}, 48000);
    const QVector<float> partial(146, 0.5f);
    const QVector<float> oversized((PcmFrame::kMaxFrames + 1) * 2, 0.0f);
    for (const QVector<float>& invalid : {QVector<float>{}, QVector<float>{0.5f},
                                        QVector<float>{0.0f, std::numeric_limits<float>::infinity()},
                                        QVector<float>{std::numeric_limits<float>::quiet_NaN(), 0.0f}, oversized}) {
        QVector<float> output;
        check(append(converter, view(partial), output), "partial block primed before invalid input");
        int calls = 0;
        check(!converter.process(view(invalid), [&](QVector<float>) { ++calls; return true; })
                  && calls == 0 && converter.inputFrames() == 0 && converter.stagedInputFrames() == 0,
              "invalid input emits nothing and discards preceding partial audio");
    }
    const QVector<float> maximum(PcmFrame::kMaxFrames * 2, 0.0f);
    QVector<float> output;
    check(append(converter, view(maximum), output), "65536 input frames accepted without aggregate-output allocation");
    check(converter.inputFrames() == 65536 && converter.outputFrames() == 131072,
          "24 to 48 kHz max input is delivered as bounded output blocks without truncation");
    converter.discard();
    int calls = 0;
    check(!converter.process(view(maximum), [&](QVector<float>) { return ++calls < 3; }),
          "sink rejection fails the current process");
    check(calls == 3 && converter.inputFrames() == 0 && converter.outputFrames() == 0
              && converter.stagedInputFrames() == 0,
          "sink refusal stops callbacks immediately and discards all conversion state");
    output.clear();
    const QVector<float> silence(16384, 0.0f);
    check(append(converter, view(silence), output)
              && std::ranges::all_of(output, [](float sample) { return sample == 0.0f; }),
          "sink rejection leaves a clean converter for the next stream");
    check(!converter.process(view(partial), {}), "missing output sink fails closed");

    QVector<float> lateInvalid(8192 * 2, 0.25f);
    lateInvalid.last() = std::numeric_limits<float>::quiet_NaN();
    calls = 0;
    check(!converter.process(view(lateInvalid), [&](QVector<float>) { ++calls; return true; })
              && calls == 0,
          "nonfinite last sample rejects the entire call before any earlier output escapes");

    // Finite float input can overflow float output during filter overshoot.
    // The converter must not pass that through to integer packet encoding.
    const QVector<float> huge(8192 * 2, std::numeric_limits<float>::max());
    bool finite = true;
    check(!converter.process(view(huge), [&](QVector<float> block) {
        finite &= std::ranges::all_of(block, [](float sample) { return std::isfinite(sample); });
        return true;
    }) && finite && converter.inputFrames() == 0,
          "nonfinite filter output fails closed before reaching the sink");
}
} // namespace

int main()
{
    nativeIsExact();
    rateMatrixAndPartitioning();
    antiphaseAndWideAudio();
    discardRetiresEverything();
    declaredLatencyMatchesWaveform();
    boundsAndRejection();
    std::printf("tci_rx_converter_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
