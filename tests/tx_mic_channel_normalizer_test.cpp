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
#include <limits>
#include <string>
#include <vector>

using AetherSDR::Resampler;
using AetherSDR::TxMicChannelNormalizer::AutoState;
using AetherSDR::TxMicChannelNormalizer::ChannelMode;
using AetherSDR::TxMicChannelNormalizer::Diagnostics;
using AetherSDR::TxMicChannelNormalizer::LevelBlock;
using AetherSDR::TxMicChannelNormalizer::canonicalizeFloat32ToMonoStereo;
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

std::vector<float> floatSamples(const QByteArray& bytes)
{
    std::vector<float> samples(bytes.size() / static_cast<int>(sizeof(float)));
    std::memcpy(samples.data(), bytes.constData(), bytes.size());
    return samples;
}

// Float32 capture is dyadic — every fixture below is written as n/32768 so the
// expected values are exact in binary and this stays an equality test, not an
// epsilon test.
bool floatSamplesEqual(const QByteArray& bytes, const std::vector<float>& expected)
{
    return floatSamples(bytes) == expected;
}

constexpr float q(int int16Value)
{
    return static_cast<float>(int16Value) / 32768.0f;
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

// ─── Float32 twins ───────────────────────────────────────────────────────────
//
// canonicalizeFloat32ToMonoStereo() runs on every mic callback on Windows and
// Linux once capture leads with Float32, but it inherited none of the ~15 cases
// its Int16 sibling carries. These are the same fixtures at the same levels
// (written as n/32768 so both routes are literally the same signal), so the two
// implementations are pinned against each other rather than each against itself.

void testFloat32MonoDuplicates()
{
    AutoState state;
    Diagnostics diag;
    const QByteArray out = canonicalizeFloat32ToMonoStereo(
        floatBytes({q(1000), q(-2000), q(3000)}), 1, 24000,
        ChannelMode::Auto, &state, &diag);

    report("float32 mono duplicates without level change",
           floatSamplesEqual(out, {q(1000), q(1000), q(-2000), q(-2000),
                                   q(3000), q(3000)}));
    report("float32 mono diagnostic mode is Mono",
           diag.selectedMode == ChannelMode::Mono);
}

void testFloat32StereoOneSidedKeepsFullLevel()
{
    {
        AutoState state;
        Diagnostics diag;
        const QByteArray out = canonicalizeFloat32ToMonoStereo(
            floatBytes({q(12000), 0.0f, q(-10000), 0.0f}), 2, 24000,
            ChannelMode::Auto, &state, &diag);
        report("float32 stereo left-only selects left, not half level",
               floatSamplesEqual(out, {q(12000), q(12000), q(-10000), q(-10000)}));
        report("float32 left-only diagnostic selects Left",
               diag.selectedMode == ChannelMode::Left && diag.oneSidedStereo);
    }
    {
        AutoState state;
        Diagnostics diag;
        const QByteArray out = canonicalizeFloat32ToMonoStereo(
            floatBytes({0.0f, q(9000), 0.0f, q(-11000)}), 2, 24000,
            ChannelMode::Auto, &state, &diag);
        report("float32 stereo right-only selects right, not half level",
               floatSamplesEqual(out, {q(9000), q(9000), q(-11000), q(-11000)}));
        report("float32 right-only diagnostic selects Right",
               diag.selectedMode == ChannelMode::Right && diag.oneSidedStereo);
    }
}

void testFloat32StereoAveraging()
{
    {
        AutoState state;
        Diagnostics diag;
        const QByteArray out = canonicalizeFloat32ToMonoStereo(
            floatBytes({q(6000), q(6000), q(-7000), q(-7000)}), 2, 24000,
            ChannelMode::Auto, &state, &diag);
        report("float32 stereo L=R remains unchanged, not boosted",
               floatSamplesEqual(out, {q(6000), q(6000), q(-7000), q(-7000)}));
        report("float32 equal stereo uses average mode",
               diag.selectedMode == ChannelMode::Average);
    }
    {
        AutoState state;
        Diagnostics diag;
        const QByteArray out = canonicalizeFloat32ToMonoStereo(
            floatBytes({q(10000), q(5000), q(-8000), q(-4000)}), 2, 24000,
            ChannelMode::Auto, &state, &diag);
        // The float route must NOT round here — that is the whole point of the
        // change. 7500/32768 is exact, so an Int16-style rounding step would
        // show up as an inequality rather than as drift.
        report("float32 balanced but different stereo averages without rounding",
               floatSamplesEqual(out, {q(7500), q(7500), q(-6000), q(-6000)}));
        report("float32 balanced stereo is not flagged one-sided",
               diag.selectedMode == ChannelMode::Average && !diag.oneSidedStereo);
    }
}

void testFloat32NonFiniteSamplesAreScrubbed()
{
    // Unlike the Int16 route, this one CAN carry NaN/Inf from a misbehaving
    // driver, and everything downstream is stateful DSP.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    AutoState state;
    Diagnostics diag;
    const QByteArray mono = canonicalizeFloat32ToMonoStereo(
        floatBytes({nan, q(1000), -inf}), 1, 24000,
        ChannelMode::Auto, &state, &diag);
    report("float32 mono non-finite samples become silence",
           floatSamplesEqual(mono, {0.0f, 0.0f, q(1000), q(1000), 0.0f, 0.0f}));

    AutoState stereoState;
    const QByteArray stereo = canonicalizeFloat32ToMonoStereo(
        floatBytes({nan, q(6000), q(-7000), inf}), 2, 24000,
        ChannelMode::Average, &stereoState, &diag);
    const std::vector<float> got = floatSamples(stereo);
    const bool allFinite = std::all_of(got.begin(), got.end(),
                                       [](float f) { return std::isfinite(f); });
    report("float32 stereo averaging never propagates NaN/Inf", allFinite);
}

void testFloat32OversizeAndRaggedBlocks()
{
    {
        char sentinel = 0;
        constexpr qsizetype dumpInputBytes = 0x80007900;
        const QByteArray dumpSized = QByteArray::fromRawData(&sentinel, dumpInputBytes);
        Diagnostics diag;
        const QByteArray out = canonicalizeFloat32ToMonoStereo(
            dumpSized, 2, 48000, ChannelMode::Average, nullptr, &diag);
        report("float32 dump-sized mic block is rejected without allocation",
               out.isEmpty() && diag.inputRejected);
    }
    {
        // Stereo float32 frames are 8 bytes; a 12-byte block is one whole frame
        // plus half of the next.
        const QByteArray malformed(12, '\0');
        Diagnostics diag;
        const QByteArray out = canonicalizeFloat32ToMonoStereo(
            malformed, 2, 48000, ChannelMode::Auto, nullptr, &diag);
        report("partial stereo float32 frame is truncated, whole frames survive",
               out.size() == 8 && diag.frames == 1 && !diag.inputRejected);
        report("float32 truncation is recorded in the diagnostics",
               diag.partialFrameBytes == 4 && diag.inputBytes == malformed.size());
    }
    {
        const QByteArray tooShort(4, '\0'); // half a stereo float32 frame
        Diagnostics diag;
        const QByteArray out = canonicalizeFloat32ToMonoStereo(
            tooShort, 2, 48000, ChannelMode::Auto, nullptr, &diag);
        report("a sub-frame float32 block yields nothing and is not a fault",
               out.isEmpty() && diag.frames == 0 && !diag.inputRejected);
    }
}

void testCaptureBufferAlignsToFloat32Frames()
{
    // TxCaptureBuffer.h warns that aligning float32 capture to Int16 frames
    // "slips the interleave and turns into audio that reads as a DSP fault
    // rather than a framing one". Nothing pinned it until now.
    {
        // 3-channel float32 frames are 12 bytes, which 256 KiB does not divide.
        QByteArray backlog(TxCaptureBuffer::kMaxReadBytes + 4096, 'x');
        QBuffer device(&backlog);
        device.open(QIODevice::ReadOnly);

        const TxCaptureBuffer::BoundedRead read =
            TxCaptureBuffer::readLatestBounded(&device, 3, 4);
        report("float32 bounded read is frame-aligned for 3ch (12-byte frames)",
               read.block.size() % 12 == 0
                   && read.block.size() <= TxCaptureBuffer::kMaxReadBytes
                   && read.block.size() > TxCaptureBuffer::kMaxReadBytes - 12);
    }
    {
        // The int16/float32 divergence made concrete. A 4100-byte stereo block
        // is a whole number of 4-byte Int16 frames but ends mid-frame at 8-byte
        // float32 frames, so passing the Int16 width for a float32 device is
        // exactly the mis-alignment TxCaptureBuffer.h warns about: it would hand
        // on a trailing half-frame and slip the interleave.
        QByteArray backlog(4100, 'x');
        QBuffer device(&backlog);
        device.open(QIODevice::ReadOnly);
        const qsizetype f32 = TxCaptureBuffer::readLatestBounded(&device, 2, 4).block.size();

        QByteArray backlog2(4100, 'x');
        QBuffer device2(&backlog2);
        device2.open(QIODevice::ReadOnly);
        const qsizetype i16 = TxCaptureBuffer::readLatestBoundedInt16(&device2, 2).block.size();

        report("bytesPerSample actually changes the cut (float32 != int16)",
               f32 == 4096 && i16 == 4100,
               "f32=" + std::to_string(f32) + " i16=" + std::to_string(i16));
    }
    {
        // Push-mode trim: the DISCARD is what has to land on an 8-byte float32
        // frame, because that is what decides where the retained tail STARTS. A
        // 4-byte slip there swaps L/R for the remainder of the block. (The
        // retained size may still end mid-frame; the trailing partial frame is
        // truncated downstream by canonicalizeFloat32ToMonoStereo.)
        QByteArray ragged(TxCaptureBuffer::kMaxReadBytes + 4100, 'x');
        const qint64 discarded = TxCaptureBuffer::trimToLatestBounded(ragged, 2, 4);
        report("a mid-frame float32 push block discards whole 8-byte frames",
               discarded % 8 == 0 && discarded == 4104
                   && ragged.size() <= TxCaptureBuffer::kMaxReadBytes);

        QByteArray small(4096, 'x');
        report("a float32 push block that already fits is left alone",
               TxCaptureBuffer::trimToLatestBounded(small, 2, 4) == 0
                   && small.size() == 4096);
    }
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
    testFloat32MonoDuplicates();
    testFloat32StereoOneSidedKeepsFullLevel();
    testFloat32StereoAveraging();
    testFloat32NonFiniteSamplesAreScrubbed();
    testFloat32OversizeAndRaggedBlocks();
    testCaptureBufferAlignsToFloat32Frames();

    std::printf("\n%s\n", g_failed == 0 ? "All tests passed." : "Some tests failed.");
    return g_failed == 0 ? 0 : 1;
}
