// RFC #5468 A3: immutable-format conversion without files, settings, devices,
// sockets, a radio or TX. Every stream is serialized on this calling thread.
#include "core/QsoPcmConverter.h"

#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numbers>
#include <optional>
#include <vector>

using namespace AetherSDR;

namespace {
using Input = QsoPcmConverter::InputEncoding;
using Output = QsoPcmConverter::OutputEncoding;
using Configuration = QsoPcmConverter::Configuration;
constexpr double kTau = 2.0 * std::numbers::pi;
int checks = 0;
int failures = 0;

void check(bool condition, const char* message)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

QByteArray floatBytes(const std::vector<float>& values)
{
    return {reinterpret_cast<const char*>(values.data()),
            static_cast<qsizetype>(values.size() * sizeof(float))};
}

QByteArray pcmBytes(const std::vector<qint16>& values, Input encoding)
{
    QByteArray result(static_cast<qsizetype>(values.size() * sizeof(qint16)), Qt::Uninitialized);
    for (std::size_t index = 0; index < values.size(); ++index) {
        char* output = result.data() + index * sizeof(qint16);
        if (encoding == Input::Int16LittleEndian) {
            qToLittleEndian<qint16>(values[index], output);
        } else {
            std::memcpy(output, &values[index], sizeof(qint16));
        }
    }
    return result;
}

std::vector<float> unpack(QByteArrayView data, Output encoding)
{
    const int sampleBytes = encoding == Output::Float32Native ? sizeof(float) : sizeof(qint16);
    std::vector<float> values(static_cast<std::size_t>(data.size() / sampleBytes));
    for (std::size_t index = 0; index < values.size(); ++index) {
        const char* sample = data.data() + index * sampleBytes;
        if (encoding == Output::Float32Native) {
            std::memcpy(&values[index], sample, sizeof(float));
        } else {
            qint16 integer;
            if (encoding == Output::Int16LittleEndian) {
                integer = qFromLittleEndian<qint16>(sample);
            } else {
                std::memcpy(&integer, sample, sizeof(integer));
            }
            values[index] = integer / 32768.0f;
        }
    }
    return values;
}

QByteArray encode(const std::vector<float>& values, Input encoding)
{
    if (encoding == Input::Float32Native) {
        return floatBytes(values);
    }
    std::vector<qint16> integers(values.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
        integers[index] = static_cast<qint16>(std::clamp(values[index] * 32768.0f,
                                                       -32768.0f, 32767.0f));
    }
    return pcmBytes(integers, encoding);
}

std::vector<float> tones(int rate, int frames, int channels,
                         double leftHz = 700, double rightHz = 1700)
{
    std::vector<float> values(static_cast<std::size_t>(frames * channels));
    for (int frame = 0; frame < frames; ++frame) {
        values[static_cast<std::size_t>(frame * channels)] =
            static_cast<float>(0.3 * std::sin(kTau * leftHz * frame / rate));
        if (channels == 2) {
            values[static_cast<std::size_t>(frame * channels + 1)] =
                static_cast<float>(0.17 * std::sin(kTau * rightHz * frame / rate));
        }
    }
    return values;
}

double amplitude(const std::vector<float>& values, int rate, int channels,
                 int channel, double frequency)
{
    const int frames = static_cast<int>(values.size()) / channels;
    const int begin = frames / 4;
    const int count = frames / 2;
    double real = 0;
    double imaginary = 0;
    for (int frame = begin; frame < begin + count; ++frame) {
        const double phase = kTau * frequency * frame / rate;
        const float sample = values[static_cast<std::size_t>(frame * channels + channel)];
        real += sample * std::cos(phase);
        imaginary += sample * std::sin(phase);
    }
    return 2.0 * std::hypot(real, imaginary) / count;
}

std::optional<QByteArray> run(const Configuration& configuration, QByteArrayView input,
                            const std::vector<int>& chunks)
{
    QsoPcmConverter converter(configuration);
    check(converter.isValid(), "test stream configuration is valid");
    if (!converter.isValid()) {
        return std::nullopt;
    }
    const int inputSampleBytes = configuration.sourceEncoding == Input::Float32Native
        ? sizeof(float) : sizeof(qint16);
    const int inputFrameBytes = inputSampleBytes * configuration.sourceChannels;
    const int outputSampleBytes = configuration.outputEncoding == Output::Float32Native
        ? sizeof(float) : sizeof(qint16);
    QByteArray result;
    std::size_t chunkIndex = 0;
    for (qsizetype offset = 0; offset < input.size();) {
        const qsizetype bytes = std::min<qsizetype>(input.size() - offset,
            chunks[chunkIndex++ % chunks.size()] * inputFrameBytes);
        QByteArray block;
        if (!converter.process(input.sliced(offset, bytes), block)) {
            check(false, "valid streaming block accepted");
            return std::nullopt;
        }
        result.append(block);
        offset += bytes;
    }
    QByteArray tail;
    const bool drained = converter.finish(tail);
    if (!drained) {
        std::fprintf(stderr, "drain detail %d>%d source %llu emitted %llu target %llu\n",
            configuration.sourceRate, configuration.outputRate,
            static_cast<unsigned long long>(converter.inputFrames()),
            static_cast<unsigned long long>(converter.outputFrames()),
            static_cast<unsigned long long>(converter.targetOutputFrames()));
    }
    check(drained, "finite stream drains successfully");
    result.append(tail);
    const std::uint64_t sourceFrames = static_cast<std::uint64_t>(input.size() / inputFrameBytes);
    const std::uint64_t expectedFrames =
        (sourceFrames * configuration.outputRate + configuration.sourceRate / 2)
        / configuration.sourceRate;
    check(converter.inputFrames() == sourceFrames, "source cursor counts complete frames");
    check(converter.outputFrames() == expectedFrames, "converter frame count has exact rational duration");
    check(static_cast<std::uint64_t>(result.size())
              == expectedFrames * configuration.outputChannels * outputSampleBytes,
          "packed payload has independently calculated duration");
    check(converter.finished(), "finish seals the source");
    check(converter.finish(tail) && tail.isEmpty(), "repeated finish cannot append duration");
    return result;
}

void exactLegacyAndPcm()
{
    const std::vector<float> samples{
        -std::numeric_limits<float>::max(), -2, -1, -0.5f, -0.0f,
        0.0f, 0.5f, 1, 2, std::numeric_limits<float>::max()
    };
    const auto legacy = run({24000, 2, Input::Float32Native, 24000, 2, Output::Int16LittleEndian},
                            floatBytes(samples), {1, 2});
    check(legacy.has_value(), "legacy RX converts");
    if (legacy) {
        const std::vector<qint16> expected{-32767, -32767, -32767, -16383, 0,
                                          0, 16383, 32767, 32767, 32767};
        check(*legacy == pcmBytes(expected, Input::Int16LittleEndian),
              "legacy RX retains clipping, multiply 32767 and truncation exactly");
    }

    // Every signed 16-bit value, including -32768, must survive equal-rate
    // voice/CW or existing-file conversion with no requantization loss.
    std::vector<qint16> allValues(65536);
    for (int value = -32768; value <= 32767; ++value) {
        allValues[static_cast<std::size_t>(value + 32768)] = static_cast<qint16>(value);
    }
    for (Input encoding : {Input::Int16Native, Input::Int16LittleEndian}) {
        const QByteArray input = pcmBytes(allValues, encoding);
        for (Output output : {Output::Int16Native, Output::Int16LittleEndian, Output::Float32Native}) {
            const auto converted = run({24000, 1, encoding, 24000, 1, output}, input, {65536});
            if (!converted) {
                continue;
            }
            const std::vector<float> decoded = unpack(*converted, output);
            bool exact = decoded.size() == allValues.size();
            for (std::size_t index = 0; index < decoded.size(); ++index) {
                exact = exact && decoded[index] == allValues[index] / 32768.0f;
            }
            check(exact, "all PCM16 bit patterns survive equal-rate native/LE/Float packing");
        }
    }
}

void rateMatrixAndChannels()
{
    for (Input encoding : {Input::Float32Native, Input::Int16Native, Input::Int16LittleEndian}) {
        for (int sourceRate : {24000, 44100, 48000}) {
            if (sourceRate == 44100 && encoding == Input::Float32Native) {
                continue;
            }
            for (int outputRate : {24000, 44100, 48000}) {
                for (Output outputEncoding : {Output::Int16LittleEndian, Output::Int16Native,
                                              Output::Float32Native}) {
                    const QByteArray input = encode(tones(sourceRate, sourceRate, 2), encoding);
                    const Configuration configuration{sourceRate, 2, encoding,
                                                      outputRate, 2, outputEncoding};
                    const auto single = run(configuration, input, {65536});
                    const auto split = run(configuration, input, {1, 2, 17, 511, 7, 4097, 3});
                    if (!single || !split) {
                        continue;
                    }
                    check(*single == *split, "output bits and duration are independent of input block sizes");
                    const std::vector<float> samples = unpack(*split, outputEncoding);
                    check(std::abs(amplitude(samples, outputRate, 2, 0, 700) - 0.3) < 0.008,
                          "left tone retains pitch and level at negotiated sink rate");
                    check(std::abs(amplitude(samples, outputRate, 2, 1, 1700) - 0.17) < 0.008,
                          "right tone retains pitch and level at negotiated sink rate");
                    check(amplitude(samples, outputRate, 2, 0, 1700) < 0.002
                              && amplitude(samples, outputRate, 2, 1, 700) < 0.002,
                          "independent L/R resamplers prevent stereo downmix crosstalk");

                    const auto mono = run({sourceRate, 2, encoding, outputRate, 1, outputEncoding},
                                          input, {511, 17});
                    if (mono) {
                        const std::vector<float> monoSamples = unpack(*mono, outputEncoding);
                        check(std::abs(amplitude(monoSamples, outputRate, 1, 0, 700) - 0.15) < 0.008
                                  && std::abs(amplitude(monoSamples, outputRate, 1, 0, 1700) - 0.085) < 0.008,
                              "mono sink averages independently converted stereo channels");
                    }
                }
                const QByteArray monoInput = encode(tones(sourceRate, 2047, 1), encoding);
                const auto duplicated = run({sourceRate, 1, encoding, outputRate, 2, Output::Float32Native},
                                            monoInput, {1, 13, 997});
                if (duplicated) {
                    const std::vector<float> samples = unpack(*duplicated, Output::Float32Native);
                    bool same = true;
                    for (std::size_t index = 0; index < samples.size(); index += 2) {
                        same = same && samples[index] == samples[index + 1];
                    }
                    check(same, "mono producer duplicates sample-exactly into stereo");
                }
            }
        }
    }

    const QByteArray wideInput = floatBytes(tones(48000, 48000, 2, 3000, 16000));
    const auto narrow = run({48000, 2, Input::Float32Native, 24000, 2, Output::Float32Native},
                            wideInput, {1023});
    if (narrow) {
        const std::vector<float> samples = unpack(*narrow, Output::Float32Native);
        check(amplitude(samples, 24000, 2, 0, 3000) > 0.29,
              "48 to 24 conversion retains in-band content");
        check(amplitude(samples, 24000, 2, 1, 8000) < 0.001,
              "48 to 24 conversion filters content above destination Nyquist");
    }
}

void shortStreamsAndTails()
{
    for (int sourceRate : {24000, 44100, 48000}) {
        for (int outputRate : {24000, 44100, 48000}) {
            for (int frames : {1, 2, 3, 5, 17, 127, 511}) {
                std::vector<float> impulses(static_cast<std::size_t>(frames * 2), 0.0f);
                impulses[0] = 0.5f;
                impulses[static_cast<std::size_t>(frames * 2 - 1)] = -0.5f;
                const Configuration configuration{sourceRate, 2, Input::Int16LittleEndian,
                                                  outputRate, 2, Output::Float32Native};
                const QByteArray input = encode(impulses, configuration.sourceEncoding);
                const auto single = run(configuration, input, {65536});
                const auto split = run(configuration, input, {1, 3, 7});
                if (!single || !split) {
                    continue;
                }
                check(*single == *split, "short-file conversion is independent of tiny input blocks");
                const std::vector<float> samples = unpack(*single, Output::Float32Native);
                check(!samples.empty() && samples.front() > 0.1f,
                      "short file starts with real impulse content after latency trimming");
                float lastImpulse = 0.0f;
                const std::size_t tailFrames = static_cast<std::size_t>(
                    (2 * outputRate + sourceRate - 1) / sourceRate);
                const std::size_t firstTailSample = samples.size() > tailFrames * 2
                    ? samples.size() - tailFrames * 2 : 0;
                for (std::size_t index = firstTailSample + 1; index < samples.size(); index += 2) {
                    lastImpulse = std::min(lastImpulse, samples[index]);
                }
                check(lastImpulse < -0.01f,
                      "finite drain preserves the final real impulse within source duration");
            }
        }
    }
}

void preferredDeviceRates()
{
    // AudioFormatNegotiator can fall back to a device's preferred rate, even
    // outside the customary 24/44.1/48 kHz matrix. The coprime cases exercise
    // phase alignment where the prefix needs nearly a source-rate second.
    for (int sourceRate : {24000, 44100, 48000}) {
        for (int outputRate : {8000, 8001, 96000, 95999, 192000}) {
            const Configuration configuration{sourceRate, 2, Input::Int16LittleEndian,
                                              outputRate, 2, Output::Float32Native};
            const QByteArray input = encode(tones(sourceRate, sourceRate, 2), Input::Int16LittleEndian);
            const auto single = run(configuration, input, {65536});
            const auto split = run(configuration, input, {1, 17, 4097});
            if (single && split) {
                check(*single == *split, "preferred sink rate is independent of source block partition");
                const std::vector<float> samples = unpack(*single, Output::Float32Native);
                check(std::abs(amplitude(samples, outputRate, 2, 0, 700) - 0.3) < 0.008
                          && std::abs(amplitude(samples, outputRate, 2, 1, 1700) - 0.17) < 0.008,
                      "preferred 8k/96k/192k and coprime sink rates preserve stereo pitch and duration");
                check(amplitude(samples, outputRate, 2, 0, 1700) < 0.002
                          && amplitude(samples, outputRate, 2, 1, 700) < 0.002,
                      "preferred sink rates preserve independent channels");
            }
            std::vector<float> impulse(34, 0.0f);
            impulse[0] = 0.5f;
            const auto shortFile = run(configuration, encode(impulse, Input::Int16LittleEndian), {1, 3});
            if (shortFile) {
                const std::vector<float> samples = unpack(*shortFile, Output::Float32Native);
                check(!samples.empty() && samples.front() > 0.05f,
                      "preferred sink rate retains real content of a short file");
            }
        }
    }

    const QByteArray maximum = floatBytes(tones(24000, QsoPcmConverter::kMaxInputFrames, 2));
    const auto largest = run({24000, 2, Input::Float32Native, 192000, 2, Output::Float32Native},
                             maximum, {QsoPcmConverter::kMaxInputFrames});
    check(largest.has_value(), "maximum admitted block safely expands by maximum supported ratio");
}

void rejectionAndRetirement()
{
    const Configuration valid{24000, 2, Input::Float32Native, 48000, 2, Output::Int16LittleEndian};
    for (int invalidCase = 0; invalidCase < 8; ++invalidCase) {
        Configuration invalid = valid;
        switch (invalidCase) {
        case 0: invalid.sourceRate = 44100; break;
        case 1: invalid.sourceRate = 0; break;
        case 2: invalid.outputRate = 192001; break;
        case 3: invalid.sourceChannels = 3; break;
        case 4: invalid.outputChannels = 0; break;
        case 5: invalid.sourceEncoding = static_cast<Input>(99); break;
        case 6: invalid.outputEncoding = static_cast<Output>(99); break;
        case 7: invalid.outputRate = 7999; break;
        }
        QsoPcmConverter converter(invalid);
        QByteArray output("preserved");
        check(!converter.isValid() && !converter.process(floatBytes({0, 0}), output)
                  && !converter.finish(output) && output == "preserved",
              "invalid formats fail closed without modifying output");
    }

    QsoPcmConverter converter(valid);
    QsoPcmConverter reference(valid);
    const QByteArray initial = floatBytes(tones(24000, 7, 2));
    QByteArray first;
    QByteArray expectedFirst;
    check(converter.process(initial, first) && reference.process(initial, expectedFirst),
          "matching sources accept initial history");
    std::vector<QByteArray> invalidBlocks{QByteArray{}, QByteArray(7, '\0'),
        QByteArray((QsoPcmConverter::kMaxInputFrames + 1) * 8, '\0'),
        floatBytes({0, 0, 0, std::numeric_limits<float>::quiet_NaN()}),
        floatBytes({0, std::numeric_limits<float>::infinity()}),
        floatBytes({-std::numeric_limits<float>::infinity(), 0})};
    for (const QByteArray& invalid : invalidBlocks) {
        QByteArray output("unchanged");
        const std::uint64_t oldInput = converter.inputFrames();
        const std::uint64_t oldOutput = converter.outputFrames();
        check(!converter.process(invalid, output) && output == "unchanged"
                  && converter.inputFrames() == oldInput && converter.outputFrames() == oldOutput,
              "invalid input preserves output, source cursor and emitted cursor");
    }
    QByteArray next;
    QByteArray expectedNext;
    QByteArray tail;
    QByteArray expectedTail;
    const QByteArray continuation = floatBytes(tones(24000, 8191, 2));
    check(converter.process(continuation, next) && reference.process(continuation, expectedNext)
              && converter.finish(tail) && reference.finish(expectedTail)
              && first == expectedFirst && next == expectedNext && tail == expectedTail,
          "invalid complete-block validation never perturbs resampler history");

    for (Input encoding : {Input::Int16Native, Input::Int16LittleEndian}) {
        for (int channels : {1, 2}) {
            QsoPcmConverter pcmConverter({44100, channels, encoding,
                                           48000, 2, Output::Float32Native});
            QByteArray pcmOutput("unchanged");
            for (const QByteArray& malformed : {
                     QByteArray(1, '\0'), QByteArray(channels * 2 - 1, '\0'),
                     QByteArray((QsoPcmConverter::kMaxInputFrames + 1) * channels * 2, '\0')}) {
                check(!pcmConverter.process(malformed, pcmOutput)
                          && pcmConverter.inputFrames() == 0 && pcmOutput == "unchanged",
                      "PCM16 rejects partial and oversized mono/stereo frames before admission");
            }
        }
    }
    QByteArray sealed("sealed");
    check(!converter.process(initial, sealed) && sealed == "sealed",
          "finished source refuses further input");

    QsoPcmConverter retired(valid);
    QByteArray output;
    check(retired.process(initial, output) && output.isEmpty(),
          "a short streaming source retains bounded filter latency until finish");
    retired.discard();
    check(retired.finished() && !retired.finish(output) && !retired.process(initial, output),
          "discarded source cannot append its delayed tail or resume");
    const auto replacement = run(valid, floatBytes(std::vector<float>(8192, 0.0f)), {17, 1, 4095});
    check(replacement && *replacement == QByteArray(replacement->size(), '\0'),
          "replacement source has no retired filter history");

    const QByteArray aligned = floatBytes({0.25f, -0.75f});
    const QByteArray unaligned = QByteArray("x") + aligned;
    QsoPcmConverter unalignedConverter({24000, 2, Input::Float32Native,
                                       24000, 2, Output::Float32Native});
    check(unalignedConverter.process(QByteArrayView(unaligned).sliced(1), output) && output == aligned,
          "native sample ingress does not require aligned memory");

    for (int sourceRate : {24000, 48000}) {
        for (int outputRate : {24000, 44100, 48000}) {
            std::vector<float> peaks(2 * 2048, std::numeric_limits<float>::max());
            for (std::size_t index = 1; index < peaks.size(); index += 2) {
                peaks[index] = -peaks[index];
            }
            for (Output encoding : {Output::Float32Native, Output::Int16Native}) {
                const auto packed = run({sourceRate, 2, Input::Float32Native,
                                         outputRate, 2, encoding}, floatBytes(peaks), {17, 511});
                if (packed) {
                    const std::vector<float> values = unpack(*packed, encoding);
                    check(std::all_of(values.begin(), values.end(), [](float value) {
                        return std::isfinite(value) && value >= -1.0f && value <= 1.0f;
                    }), "extreme finite input and filter overshoot stay bounded in Float and Int16 sinks");
                }
            }
        }
    }

    QsoPcmConverter empty(valid);
    check(empty.finish(output) && output.isEmpty() && empty.outputFrames() == 0,
          "empty finite source does not emit a resampler silence tail");
}

} // namespace

int main()
{
    exactLegacyAndPcm();
    rateMatrixAndChannels();
    shortStreamsAndTails();
    preferredDeviceRates();
    rejectionAndRetirement();
    std::printf("qso_recorder_conversion_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
