// Standalone tests for TX mic channel canonicalization.
// Run: ./build/tx_mic_channel_normalizer_test

#include "core/Resampler.h"
#include "core/TxCaptureBuffer.h"
#include "core/TxMicChannelNormalizer.h"

#include <QByteArray>
#include <QBuffer>
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
namespace TxCaptureBuffer = AetherSDR::TxCaptureBuffer;

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

void testCaptureReadIsBounded()
{
    QByteArray backlog(TxCaptureBuffer::kMaxReadBytes + 4096, 'x');
    QBuffer device(&backlog);
    device.open(QIODevice::ReadOnly);

    const TxCaptureBuffer::BoundedRead read =
        TxCaptureBuffer::readLatestBoundedInt16(&device, 2);
    report("pull-mode capture reads at most one bounded block",
           read.block.size() == TxCaptureBuffer::kMaxReadBytes);
    report("a within-catch-up residue is left for later callbacks, not dropped",
           device.bytesAvailable() == 4096 && read.discardedBytes == 0);
}

void testCaptureReadDropsToLatestPastCatchUpThreshold()
{
    // The freshest block must be the one transmitted: mark the backlog so the
    // returned bytes can only have come from its tail.
    const qint64 backlogBytes = TxCaptureBuffer::kCatchUpThresholdBytes + 64 * 1024;
    QByteArray backlog(backlogBytes, 's');           // stale head
    backlog.replace(backlogBytes - TxCaptureBuffer::kMaxReadBytes,
                    TxCaptureBuffer::kMaxReadBytes,
                    QByteArray(TxCaptureBuffer::kMaxReadBytes, 'f'));  // fresh tail
    QBuffer device(&backlog);
    device.open(QIODevice::ReadOnly);

    const TxCaptureBuffer::BoundedRead read =
        TxCaptureBuffer::readLatestBoundedInt16(&device, 2);

    report("catch-up returns one bounded block",
           read.block.size() == TxCaptureBuffer::kMaxReadBytes);
    report("catch-up transmits the newest audio, not the oldest",
           read.block == QByteArray(TxCaptureBuffer::kMaxReadBytes, 'f'));
    report("catch-up reports the stale bytes it skipped",
           read.discardedBytes == backlogBytes - TxCaptureBuffer::kMaxReadBytes);
    report("catch-up leaves the device drained",
           device.bytesAvailable() == 0);
}

void testCaptureReadAlignsToRealChannelCount()
{
    // 6-channel Int16 frames are 12 bytes, which 256 KiB does not divide: the
    // read must round down so later reads stay on frame boundaries.
    QByteArray backlog(TxCaptureBuffer::kMaxReadBytes + 4096, 'x');
    QBuffer device(&backlog);
    device.open(QIODevice::ReadOnly);

    const TxCaptureBuffer::BoundedRead read =
        TxCaptureBuffer::readLatestBoundedInt16(&device, 6);
    report("bounded read is frame-aligned for the negotiated channel count",
           read.block.size() % 12 == 0 && read.block.size() <= TxCaptureBuffer::kMaxReadBytes
               && read.block.size() > TxCaptureBuffer::kMaxReadBytes - 12);
}

void testDumpSizedMicBlockIsRejectedBeforeDereference()
{
    char sentinel = 0;
    constexpr qsizetype dumpInputBytes = 0x80007900;
    const QByteArray dumpSized = QByteArray::fromRawData(&sentinel, dumpInputBytes);
    Diagnostics diag;

    const QByteArray out = canonicalizeInt16ToMonoStereo(
        dumpSized,
        2,
        48000,
        ChannelMode::Average,
        nullptr,
        &diag);

    report("2 GiB dump-sized mic block is rejected without allocation",
           out.isEmpty() && diag.inputRejected);
    report("rejection diagnostics preserve the dump byte count",
           diag.inputBytes == dumpInputBytes && diag.frames == 0);
}

void testMisalignedMicBlockIsTruncatedToFrameBoundary()
{
    // A trailing partial frame costs one sample, not the whole block: dropping
    // the block would be an audible dropout, and a device that stays misaligned
    // would drop every block thereafter.
    const QByteArray malformed(6, '\0'); // stereo Int16 requires 4-byte frames
    Diagnostics diag;
    const QByteArray out = canonicalizeInt16ToMonoStereo(
        malformed,
        2,
        48000,
        ChannelMode::Auto,
        nullptr,
        &diag);

    report("partial stereo Int16 frame is truncated, whole frames survive",
           out.size() == 4 && diag.frames == 1 && !diag.inputRejected);
    report("truncation is recorded in the diagnostics",
           diag.partialFrameBytes == 2 && diag.inputBytes == malformed.size());
}

void testShortBlockBelowOneFrameIsDroppedQuietly()
{
    const QByteArray tooShort(2, '\0'); // less than one stereo Int16 frame
    Diagnostics diag;
    const QByteArray out = canonicalizeInt16ToMonoStereo(
        tooShort,
        2,
        48000,
        ChannelMode::Auto,
        nullptr,
        &diag);

    report("a sub-frame block yields nothing and is not flagged a fault",
           out.isEmpty() && diag.frames == 0 && !diag.inputRejected);
}

void testPushModeBlockIsTrimmedToLatest()
{
    // macOS hands over everything accumulated since the last 5 ms poll, so a
    // stalled audio thread produces one oversized block.
    QByteArray block(TxCaptureBuffer::kMaxReadBytes, 's');
    block.append(QByteArray(TxCaptureBuffer::kMaxReadBytes, 'f'));  // newest tail

    const qint64 discarded = TxCaptureBuffer::trimToLatestBoundedInt16(block, 2);
    report("push-mode stall is trimmed to one bounded block",
           block.size() == TxCaptureBuffer::kMaxReadBytes
               && discarded == TxCaptureBuffer::kMaxReadBytes);
    report("push-mode trim keeps the newest audio",
           block == QByteArray(TxCaptureBuffer::kMaxReadBytes, 'f'));

    QByteArray small(4096, 'x');
    report("a push-mode block that already fits is left alone",
           TxCaptureBuffer::trimToLatestBoundedInt16(small, 2) == 0
               && small.size() == 4096);

    // A push buffer that ended mid-frame must not shift the retained tail by a
    // sample, which would swap L/R for the rest of the block.
    QByteArray ragged(TxCaptureBuffer::kMaxReadBytes + 4098, 'x');
    const qint64 raggedDiscard = TxCaptureBuffer::trimToLatestBoundedInt16(ragged, 2);
    report("a mid-frame push block is cut on a frame boundary",
           raggedDiscard % 4 == 0 && ragged.size() <= TxCaptureBuffer::kMaxReadBytes);
}

void testLargeUpsampledDaxBlockIsAccepted()
{
    // A conforming 8 kHz TCI client can send a 64 KiB int16 mono frame, which
    // expands ~12x to ~768 KiB of stereo float32 before reaching this collapse
    // (#3306). The mic capture chunk would have rejected it outright, so the
    // float route carries its own, larger ceiling.
    constexpr qsizetype worstCaseTciExpansion = 768 * 1024;
    const QByteArray large(worstCaseTciExpansion, '\0');
    Diagnostics diag;
    const QByteArray out = collapseFloat32ToInt16MonoBigEndian(
        large,
        2,
        24000,
        ChannelMode::Average,
        nullptr,
        &diag);

    report("worst-case upsampled TCI block is not rejected",
           !out.isEmpty() && !diag.inputRejected
               && diag.frames == worstCaseTciExpansion / 8);
}

void testOversizedDaxBlockIsRejected()
{
    // Past its own ceiling the float route still rejects before dereference.
    char sentinel = 0;
    const QByteArray oversized = QByteArray::fromRawData(
        &sentinel, AetherSDR::TxMicChannelNormalizer::kMaxRealtimeFloatBlockBytes + 8);
    Diagnostics diag;
    const QByteArray out = collapseFloat32ToInt16MonoBigEndian(
        oversized,
        2,
        24000,
        ChannelMode::Average,
        nullptr,
        &diag);

    report("oversized DAX float32 block is rejected before dereference",
           out.isEmpty() && diag.inputRejected && diag.frames == 0);
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
    testCaptureReadIsBounded();
    testCaptureReadDropsToLatestPastCatchUpThreshold();
    testCaptureReadAlignsToRealChannelCount();
    testDumpSizedMicBlockIsRejectedBeforeDereference();
    testMisalignedMicBlockIsTruncatedToFrameBoundary();
    testShortBlockBelowOneFrameIsDroppedQuietly();
    testPushModeBlockIsTrimmedToLatest();
    testLargeUpsampledDaxBlockIsAccepted();
    testOversizedDaxBlockIsRejected();

    std::printf("\n%s\n", g_failed == 0 ? "All tests passed." : "Some tests failed.");
    return g_failed == 0 ? 0 : 1;
}
