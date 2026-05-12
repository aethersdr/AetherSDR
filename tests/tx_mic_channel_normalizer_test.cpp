// Standalone tests for TX mic channel canonicalization.
// Run: ./build/tx_mic_channel_normalizer_test

#include "core/Resampler.h"
#include "core/TxMicChannelNormalizer.h"

#include <QByteArray>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using AetherSDR::Resampler;
using AetherSDR::TxMicChannelNormalizer::AutoState;
using AetherSDR::TxMicChannelNormalizer::ChannelMode;
using AetherSDR::TxMicChannelNormalizer::Diagnostics;
using AetherSDR::TxMicChannelNormalizer::LevelBlock;
using AetherSDR::TxMicChannelNormalizer::canonicalizeInt16ToMonoStereo;
using AetherSDR::TxMicChannelNormalizer::collapseFloat32ToInt16MonoBigEndian;
using AetherSDR::TxMicChannelNormalizer::measureInt16StereoLevelBlock;
using AetherSDR::TxMicChannelNormalizer::rmsFromLevelBlock;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-68s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) ++g_failed;
}

QByteArray int16Bytes(const std::vector<int16_t>& samples)
{
    QByteArray bytes(static_cast<int>(samples.size() * sizeof(int16_t)), Qt::Uninitialized);
    std::memcpy(bytes.data(), samples.data(), bytes.size());
    return bytes;
}

std::vector<int16_t> int16Samples(const QByteArray& bytes)
{
    std::vector<int16_t> samples(bytes.size() / static_cast<int>(sizeof(int16_t)));
    std::memcpy(samples.data(), bytes.constData(), bytes.size());
    return samples;
}

QByteArray floatBytes(const std::vector<float>& samples)
{
    QByteArray bytes(static_cast<int>(samples.size() * sizeof(float)), Qt::Uninitialized);
    std::memcpy(bytes.data(), samples.data(), bytes.size());
    return bytes;
}

std::vector<int16_t> bigEndianInt16Samples(const QByteArray& bytes)
{
    std::vector<int16_t> samples(bytes.size() / static_cast<int>(sizeof(qint16)));
    const auto* src = reinterpret_cast<const qint16*>(bytes.constData());
    for (size_t i = 0; i < samples.size(); ++i) {
        samples[i] = qFromBigEndian(src[i]);
    }
    return samples;
}

bool samplesEqual(const QByteArray& bytes, const std::vector<int16_t>& expected)
{
    return int16Samples(bytes) == expected;
}

float blockRms(const QByteArray& stereo)
{
    return rmsFromLevelBlock(measureInt16StereoLevelBlock(stereo));
}

QByteArray resampleCanonicalStereoTo24k(const QByteArray& canonicalStereo, int sourceRate)
{
    const auto* src = reinterpret_cast<const int16_t*>(canonicalStereo.constData());
    const int frames = canonicalStereo.size() / static_cast<int>(2 * sizeof(int16_t));
    std::vector<float> mono(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        mono[static_cast<size_t>(i)] = src[i * 2] / 32768.0f;
    }

    Resampler resampler(sourceRate, 24000, 16384);
    QByteArray f32 = resampler.processMonoToStereo(mono.data(), frames);
    const auto* fsrc = reinterpret_cast<const float*>(f32.constData());
    const int floats = f32.size() / static_cast<int>(sizeof(float));
    QByteArray out(floats * static_cast<int>(sizeof(int16_t)), Qt::Uninitialized);
    auto* dst = reinterpret_cast<int16_t*>(out.data());
    for (int i = 0; i < floats; ++i) {
        dst[i] = static_cast<int16_t>(
            std::clamp(fsrc[i] * 32768.0f, -32768.0f, 32767.0f));
    }
    return out;
}

std::vector<int16_t> sineBlock(int frames, int sampleRate, int amplitude)
{
    std::vector<int16_t> samples(static_cast<size_t>(frames));
    constexpr double pi = 3.14159265358979323846;
    for (int i = 0; i < frames; ++i) {
        const double phase = 2.0 * pi * 1000.0 * i / sampleRate;
        samples[static_cast<size_t>(i)] = static_cast<int16_t>(std::sin(phase) * amplitude);
    }
    return samples;
}

} // namespace

void testMono24kDuplicates()
{
    AutoState state;
    Diagnostics diag;
    const QByteArray out = canonicalizeInt16ToMonoStereo(
        int16Bytes({1000, -2000, 3000}),
        1,
        24000,
        ChannelMode::Auto,
        &state,
        &diag);

    report("mono 24 kHz duplicates without level change",
           samplesEqual(out, {1000, 1000, -2000, -2000, 3000, 3000}));
    report("mono diagnostic mode is Mono", diag.selectedMode == ChannelMode::Mono);
}

void testStereoLeftOnly24kKeepsFullLevel()
{
    AutoState state;
    Diagnostics diag;
    const QByteArray out = canonicalizeInt16ToMonoStereo(
        int16Bytes({12000, 0, -10000, 0}),
        2,
        24000,
        ChannelMode::Auto,
        &state,
        &diag);

    report("stereo left-only selects left, not half level",
           samplesEqual(out, {12000, 12000, -10000, -10000}));
    report("left-only diagnostic selects Left",
           diag.selectedMode == ChannelMode::Left && diag.oneSidedStereo);
}

void testStereoRightOnly24kKeepsFullLevel()
{
    AutoState state;
    Diagnostics diag;
    const QByteArray out = canonicalizeInt16ToMonoStereo(
        int16Bytes({0, 9000, 0, -11000}),
        2,
        24000,
        ChannelMode::Auto,
        &state,
        &diag);

    report("stereo right-only selects right, not half level",
           samplesEqual(out, {9000, 9000, -11000, -11000}));
    report("right-only diagnostic selects Right",
           diag.selectedMode == ChannelMode::Right && diag.oneSidedStereo);
}

void testStereoEqualChannelsNotBoosted()
{
    AutoState state;
    Diagnostics diag;
    const QByteArray out = canonicalizeInt16ToMonoStereo(
        int16Bytes({6000, 6000, -7000, -7000}),
        2,
        24000,
        ChannelMode::Auto,
        &state,
        &diag);

    report("stereo L=R remains unchanged, not boosted",
           samplesEqual(out, {6000, 6000, -7000, -7000}));
    report("equal stereo uses average mode", diag.selectedMode == ChannelMode::Average);
}

void testStereoBalancedDifferentChannelsAverages()
{
    AutoState state;
    Diagnostics diag;
    const QByteArray out = canonicalizeInt16ToMonoStereo(
        int16Bytes({10000, 5000, -8000, -4000}),
        2,
        24000,
        ChannelMode::Auto,
        &state,
        &diag);

    report("balanced but different stereo averages",
           samplesEqual(out, {7500, 7500, -6000, -6000}));
    report("balanced stereo is not flagged one-sided",
           diag.selectedMode == ChannelMode::Average && !diag.oneSidedStereo);
}

void testLeftOnly48kResampleMatchesMono()
{
    const std::vector<int16_t> mono = sineBlock(4800, 48000, 12000);
    std::vector<int16_t> stereoLeftOnly;
    stereoLeftOnly.reserve(mono.size() * 2);
    for (int16_t sample : mono) {
        stereoLeftOnly.push_back(sample);
        stereoLeftOnly.push_back(0);
    }

    AutoState monoState;
    AutoState stereoState;
    Diagnostics monoDiag;
    Diagnostics stereoDiag;
    const QByteArray monoCanonical = canonicalizeInt16ToMonoStereo(
        int16Bytes(mono),
        1,
        48000,
        ChannelMode::Auto,
        &monoState,
        &monoDiag);
    const QByteArray stereoCanonical = canonicalizeInt16ToMonoStereo(
        int16Bytes(stereoLeftOnly),
        2,
        48000,
        ChannelMode::Auto,
        &stereoState,
        &stereoDiag);

    const QByteArray monoResampled = resampleCanonicalStereoTo24k(monoCanonical, 48000);
    const QByteArray stereoResampled = resampleCanonicalStereoTo24k(stereoCanonical, 48000);
    const float monoRms = blockRms(monoResampled);
    const float stereoRms = blockRms(stereoResampled);
    const float ratio = monoRms > 0.0f ? stereoRms / monoRms : 0.0f;

    report("left-only 48 kHz resample is not 6 dB below mono",
           ratio > 0.98f && ratio < 1.02f,
           "ratio=" + std::to_string(ratio));
}

void testPcMicMeterSeesRightOnly()
{
    const QByteArray rawRightOnly = int16Bytes({0, 10000, 0, -10000, 0, 10000});
    const LevelBlock block = measureInt16StereoLevelBlock(rawRightOnly);
    const float expected = 10000.0f / 32768.0f;
    const float rms = rmsFromLevelBlock(block);

    report("PC mic meter helper reports right-only peak",
           std::abs(block.peak - expected) < 0.0001f);
    report("PC mic meter helper reports right-only RMS",
           std::abs(rms - expected) < 0.0001f);
}

void testOpusFrameSizingAfterNormalization()
{
    std::vector<int16_t> mono(240, 1234);
    std::vector<int16_t> stereoRightOnly;
    stereoRightOnly.reserve(480);
    for (int i = 0; i < 240; ++i) {
        stereoRightOnly.push_back(0);
        stereoRightOnly.push_back(1234);
    }

    AutoState monoState;
    AutoState stereoState;
    Diagnostics diag;
    const QByteArray monoOut = canonicalizeInt16ToMonoStereo(
        int16Bytes(mono),
        1,
        24000,
        ChannelMode::Auto,
        &monoState,
        &diag);
    const QByteArray stereoOut = canonicalizeInt16ToMonoStereo(
        int16Bytes(stereoRightOnly),
        2,
        24000,
        ChannelMode::Auto,
        &stereoState,
        &diag);

    report("mono 10 ms normalizes to 960-byte Opus frame input",
           monoOut.size() == 960);
    report("stereo 10 ms normalizes to 960-byte Opus frame input",
           stereoOut.size() == 960);
}

void testDaxRadioNativeCollapseKeepsOneSidedLevel()
{
    AutoState state;
    Diagnostics diag;
    const QByteArray out = collapseFloat32ToInt16MonoBigEndian(
        floatBytes({0.5f, 0.0f, -0.25f, 0.0f}),
        2,
        24000,
        ChannelMode::Auto,
        &state,
        &diag);

    report("DAX radio-native left-only collapse keeps full level",
           bigEndianInt16Samples(out) == std::vector<int16_t>({16383, -8191}));
    report("DAX radio-native diagnostic selects Left",
           diag.selectedMode == ChannelMode::Left && diag.oneSidedStereo);
}

int main()
{
    std::printf("TX mic channel normalizer tests\n\n");

    testMono24kDuplicates();
    testStereoLeftOnly24kKeepsFullLevel();
    testStereoRightOnly24kKeepsFullLevel();
    testStereoEqualChannelsNotBoosted();
    testStereoBalancedDifferentChannelsAverages();
    testLeftOnly48kResampleMatchesMono();
    testPcMicMeterSeesRightOnly();
    testOpusFrameSizingAfterNormalization();
    testDaxRadioNativeCollapseKeepsOneSidedLevel();

    std::printf("\n%s\n", g_failed == 0 ? "All tests passed." : "Some tests failed.");
    return g_failed == 0 ? 0 : 1;
}
