// Explicit offline diagnostic, never a default CTest or a radio/USB client.
// Usage: hd_fm_recording_probe RAW_CU8 [--program 0..7]
//        [--compressed-source SOURCE.xz]
// The input format is the pinned nrsc5 sample format: interleaved unsigned
// I/Q bytes at 1488375 complex samples/s. Decompress before invoking this tool.
#include "core/backends/rtl/HdFmIqAdapter.h"
#include "core/backends/rtl/Nrsc5FmDecoder.h"
#include "core/HdFmReception.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <nrsc5.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>

#ifndef AETHER_ENABLE_NRSC5
#error "The recording probe requires the opt-in native HD decoder target."
#endif

namespace {
using Adapter = AetherSDR::rtl::HdFmIqAdapter;
using Decoder = AetherSDR::rtl::Nrsc5FmDecoder;
using Reception = AetherSDR::rtl::HdFmRawReception;
using AudioIdentity = AetherSDR::rtl::HdFmAudioIdentity;
namespace Policy = AetherSDR::SharedCapturePolicy;
constexpr qint64 kMaximumInputBytes = 256 * 1024 * 1024;
constexpr qint64 kMaximumWallMs = 120000;
constexpr std::uint64_t kSession = 1, kRevision = 1;
static_assert(NRSC5_SAMPLE_RATE_CU8 == 1488375);
static_assert(NRSC5_SAMPLE_RATE_AUDIO == 44100);
static_assert(Adapter::kOutputRate == NRSC5_SAMPLE_RATE_NATIVE_FM);
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

QJsonValue number(std::uint64_t value) { return QJsonValue(static_cast<qint64>(value)); }
QJsonValue optionalNumber(std::optional<double> value)
{ return value && std::isfinite(*value) ? QJsonValue(*value) : QJsonValue(QJsonValue::Null); }
std::uint64_t monotonicMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
template<std::size_t N> QString text(const std::array<char, N>& bytes, int limit)
{
    const auto end = std::find(bytes.begin(), bytes.end(), '\0');
    return AetherSDR::boundedBroadcastText(QString::fromUtf8(bytes.data(), end - bytes.begin()), limit);
}
QJsonObject receptionJson(const Reception& value)
{
    QJsonArray services;
    for (const auto& service : value.services) {
        if (service.program >= 0) {
            services.append(QJsonObject{{"program", service.program}, {"name", text(service.name, 64)},
                {"audioAvailable", service.audioAvailable}});
        }
    }
    return {{"valid", value.valid}, {"synced", value.synced}, {"audioValid", value.audioValid},
        {"selectedProgram", value.selectedProgram}, {"services", services},
        {"stationName", text(value.stationName, 64)}, {"title", text(value.title, 256)},
        {"artist", text(value.artist, 128)}, {"merLowerDb", optionalNumber(value.merLowerDb)},
        {"merUpperDb", optionalNumber(value.merUpperDb)}, {"cber", optionalNumber(value.cber)},
        {"frequencyOffsetHz", optionalNumber(value.frequencyOffsetHz)},
        {"syncLossCount", number(value.syncLossCount)}, {"reacquisitionCount", number(value.reacquisitionCount)},
        {"syncDurationMs", number(value.syncDurationMs)}, {"observationSequence", number(value.observationSequence)},
        {"audioSequence", number(value.audioSequence)}, {"audioEpoch", number(value.audioEpoch)},
        {"observationMonotonicMs", number(value.observationMonotonicMs)},
        {"audioMonotonicMs", number(value.audioMonotonicMs)}};
}
void printJson(const QJsonObject& value)
{
    const QByteArray bytes = QJsonDocument(value).toJson(QJsonDocument::Indented);
    std::fwrite(bytes.constData(), 1, static_cast<std::size_t>(bytes.size()), stdout);
}
int fail(const QString& message)
{
    printJson({{"schema", 1}, {"completed", false}, {"passed", false}, {"error", message}});
    return 2;
}
std::optional<QJsonObject> fileIdentity(const QString& path, qint64 maximum)
{
    const QFileInfo info(path);
    QFile file(path);
    if (!info.isFile() || info.size() <= 0 || info.size() > maximum || !file.open(QIODevice::ReadOnly)
        || file.isSequential()) { return {}; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, 65536> bytes{};
    qint64 total = 0;
    while (total < info.size()) {
        const qint64 count = file.read(bytes.data(), std::min<qint64>(bytes.size(), info.size() - total));
        if (count <= 0) { return {}; }
        hash.addData(QByteArrayView(bytes.data(), count)); total += count;
    }
    if (!file.atEnd() || file.size() != info.size()) { return {}; }
    return QJsonObject{{"path", info.canonicalFilePath()}, {"bytes", total},
        {"sha256", QString::fromLatin1(hash.result().toHex())}};
}

struct AudioStats final : Decoder::Sink {
    std::uint64_t callbacks = 0, frames = 0, differingFrames = 0, nonzeroFrames = 0, faults = 0;
    std::uint64_t retirements = 0, epoch = 0, next = 0, lastAudioEpoch = 0;
    std::uint64_t inputFeedEnd = 0, firstAudioInput = 0, lastAudioInput = 0;
    std::uint64_t maxInputGap = 0, maxSameEpochInputGap = 0, lastAudioWall = 0, maxWallGapMs = 0;
    std::uint64_t largestCallbackFrames = 0;
    double leftEnergy = 0, rightEnergy = 0, differenceEnergy = 0, leftPeak = 0, rightPeak = 0;
    QCryptographicHash leftHash{QCryptographicHash::Sha256}, rightHash{QCryptographicHash::Sha256};
    bool decoded(const AudioIdentity& identity, std::uint64_t first,
                 std::span<const float> left, std::span<const float> right) noexcept override
    {
        const auto now = monotonicMs();
        if (identity.sessionId != kSession || identity.revision != kRevision || !identity.audioEpoch
            || !identity.producedMonotonicMs || identity.producedMonotonicMs > now
            || left.empty() || left.size() != right.size() || left.size() > 2048
            || identity.audioEpoch < epoch || (identity.audioEpoch == epoch ? first != next : first != 0)) {
            ++faults; return false;
        }
        std::array<char, 8192> leftBytes{}, rightBytes{};
        for (std::size_t i = 0; i < left.size(); ++i) {
            if (!std::isfinite(left[i]) || !std::isfinite(right[i])
                || std::abs(left[i]) > 1.0f || std::abs(right[i]) > 1.0f) { ++faults; return false; }
            const double difference = double(left[i]) - right[i];
            leftEnergy += double(left[i]) * left[i]; rightEnergy += double(right[i]) * right[i];
            differenceEnergy += difference * difference;
            leftPeak = std::max(leftPeak, std::abs(double(left[i])));
            rightPeak = std::max(rightPeak, std::abs(double(right[i])));
            differingFrames += left[i] != right[i]; nonzeroFrames += left[i] != 0 || right[i] != 0;
            const auto l = std::bit_cast<std::uint32_t>(left[i]), r = std::bit_cast<std::uint32_t>(right[i]);
            for (unsigned byte = 0; byte < 4; ++byte) {
                leftBytes[4 * i + byte] = static_cast<char>((l >> (8 * byte)) & 255);
                rightBytes[4 * i + byte] = static_cast<char>((r >> (8 * byte)) & 255);
            }
        }
        if (!callbacks) { firstAudioInput = inputFeedEnd; }
        else {
            maxInputGap = std::max(maxInputGap, inputFeedEnd - lastAudioInput);
            if (identity.audioEpoch == lastAudioEpoch) {
                maxSameEpochInputGap = std::max(maxSameEpochInputGap, inputFeedEnd - lastAudioInput);
            }
            maxWallGapMs = std::max(maxWallGapMs, now - lastAudioWall);
        }
        lastAudioInput = inputFeedEnd; lastAudioWall = now; lastAudioEpoch = identity.audioEpoch;
        epoch = identity.audioEpoch; next = first + left.size();
        ++callbacks; frames += left.size(); largestCallbackFrames = std::max<std::uint64_t>(largestCallbackFrames, left.size());
        leftHash.addData(QByteArrayView(leftBytes.data(), left.size_bytes()));
        rightHash.addData(QByteArrayView(rightBytes.data(), right.size_bytes()));
        return true;
    }
    void audioRetired(std::uint64_t value) noexcept override
    {
        if (!value || value <= epoch) { ++faults; }
        ++retirements; epoch = value; next = 0;
    }
};

struct Feed final : Adapter::Sink {
    AudioStats& audio;
    Decoder& decoder;
    std::uint64_t iqFrames = 0, calls = 0, maxFeedFrames = 0, maxCallbacks = 0, maxPcmFrames = 0;
    unsigned discoveredPrograms = 0;
    bool observedSync = false;
    Reception lastSynced, lastAudio;
    Feed(AudioStats& output, Decoder& native) : audio(output), decoder(native) {}
    bool hdIq(std::span<const float> input) noexcept override
    {
        audio.inputFeedEnd = iqFrames + input.size() / 2;
        const auto beforeCallbacks = audio.callbacks, beforeFrames = audio.frames;
        const bool accepted = decoder.feed(input, kSession, kRevision);
        iqFrames += input.size() / 2; ++calls;
        maxFeedFrames = std::max<std::uint64_t>(maxFeedFrames, input.size() / 2);
        maxCallbacks = std::max(maxCallbacks, audio.callbacks - beforeCallbacks);
        maxPcmFrames = std::max(maxPcmFrames, audio.frames - beforeFrames);
        const auto& value = decoder.reception();
        if (value.synced) { observedSync = true; lastSynced = value; }
        if (audio.callbacks != beforeCallbacks) { lastAudio = value; }
        for (const auto& service : value.services) {
            if (service.program >= 0 && service.program < 8) { discoveredPrograms |= 1u << service.program; }
        }
        return accepted;
    }
};
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (arguments.size() < 2 || arguments.size() > 6) {
        return fail(QStringLiteral("Usage: hd_fm_recording_probe RAW_CU8 [--program 0..7] [--compressed-source SOURCE.xz]"));
    }
    const QString inputPath = arguments[1];
    QString compressedPath;
    int program = 0;
    bool programSeen = false;
    for (qsizetype i = 2; i < arguments.size(); i += 2) {
        if (i + 1 == arguments.size()) { return fail(QStringLiteral("Missing option value")); }
        if (arguments[i] == QLatin1String("--program") && !programSeen) {
            bool ok = false; program = arguments[i + 1].toInt(&ok); programSeen = true;
            if (!ok || program < 0 || program > 7) { return fail(QStringLiteral("Program must be 0..7")); }
        } else if (arguments[i] == QLatin1String("--compressed-source") && compressedPath.isEmpty()) {
            compressedPath = arguments[i + 1];
        } else { return fail(QStringLiteral("Unknown or repeated option")); }
    }
    const QFileInfo inputInfo(inputPath);
    QFile input(inputPath);
    if (!inputInfo.isFile() || inputInfo.size() <= 0 || inputInfo.size() > kMaximumInputBytes
        || inputInfo.size() % 2 || !input.open(QIODevice::ReadOnly) || input.isSequential()) {
        return fail(QStringLiteral("Input must be a nonempty regular cu8 file, even byte count, at most 256 MiB"));
    }
    if (input.peek(6) == QByteArray::fromHex("fd377a585a00")) {
        return fail(QStringLiteral("Input is XZ compressed; supply the decompressed cu8 recording"));
    }
    QJsonValue compressed(QJsonValue::Null);
    if (!compressedPath.isEmpty()) {
        const auto identity = fileIdentity(compressedPath, kMaximumInputBytes);
        if (!identity) { return fail(QStringLiteral("Cannot hash bounded compressed-source file")); }
        compressed = *identity;
    }
    const auto executable = fileIdentity(QCoreApplication::applicationFilePath(), 1024LL * 1024 * 1024);
    if (!executable) { return fail(QStringLiteral("Cannot hash the running probe executable")); }
    const char* libraryVersion = nullptr;
    nrsc5_get_version(&libraryVersion);
    const auto utcStart = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QElapsedTimer timer; timer.start();
    // Zero frequency offset, with a synthetic positive reference solely to
    // satisfy shared RF geometry. This does not assert the recording's RF.
    constexpr double geometryReferenceHz = 1000000;
    const Policy::CaptureDescriptor capture{1, 1, geometryReferenceHz, NRSC5_SAMPLE_RATE_CU8,
        NRSC5_SAMPLE_RATE_CU8 * 0.45, NRSC5_SAMPLE_RATE_CU8 * 0.45};
    const Policy::SliceDescriptor footprint{0, geometryReferenceHz, -225000, 225000, 0, 3000, 3000};
    Adapter adapter(capture, footprint);
    AudioStats audio;
    Reception initial; initial.receiverEpoch = 1; initial.selectedProgram = program;
    Decoder decoder(initial, audio); // Production NativePipe, never an injected Pipe.
    if (!adapter.valid() || !decoder.valid()) { return fail(QStringLiteral("Native adapter or decoder initialization failed")); }
    Feed feed(audio, decoder);
    QCryptographicHash rawHash(QCryptographicHash::Sha256);
    std::array<char, Adapter::kMaxInput * 2> bytes{};
    std::array<std::complex<float>, Adapter::kMaxInput> samples{};
    std::uint64_t inputBytes = 0, inputFrames = 0, maxBlockCallbacks = 0, maxBlockPcmFrames = 0;
    QString error;
    while (inputBytes < static_cast<std::uint64_t>(inputInfo.size())) {
        if (timer.elapsed() > kMaximumWallMs) { error = QStringLiteral("Probe wall-time budget exceeded"); break; }
        const qint64 count = input.read(bytes.data(), std::min<qint64>(bytes.size(), inputInfo.size() - inputBytes));
        if (count <= 0 || count % 2) { error = QStringLiteral("Read error or truncated IQ pair"); break; }
        rawHash.addData(QByteArrayView(bytes.data(), count));
        const std::size_t frames = static_cast<std::size_t>(count / 2);
        for (std::size_t i = 0; i < frames; ++i) {
            // Match pinned upstream U8_F exactly, including its center of 127.
            samples[i] = {(static_cast<unsigned char>(bytes[2 * i]) - 127.0f) / 128.0f,
                          (static_cast<unsigned char>(bytes[2 * i + 1]) - 127.0f) / 128.0f};
        }
        const auto beforeCallbacks = audio.callbacks, beforeFrames = audio.frames;
        const bool accepted = adapter.process(inputFrames, std::span(samples).first(frames), feed);
        inputBytes += count; inputFrames += frames;
        maxBlockCallbacks = std::max(maxBlockCallbacks, audio.callbacks - beforeCallbacks);
        maxBlockPcmFrames = std::max(maxBlockPcmFrames, audio.frames - beforeFrames);
        if (!accepted) { error = QStringLiteral("Production adapter or decoder refused input"); break; }
    }
    const bool completed = error.isEmpty() && inputBytes == static_cast<std::uint64_t>(inputInfo.size())
        && input.atEnd() && input.size() == inputInfo.size();
    const bool passed = completed && adapter.valid() && decoder.valid() && feed.observedSync
        && audio.frames && audio.nonzeroFrames && !audio.faults;
    if (!completed && error.isEmpty()) { error = QStringLiteral("Recording changed while reading"); }
    QJsonArray programs;
    for (int i = 0; i < 8; ++i) { if (feed.discoveredPrograms & (1u << i)) { programs.append(i); } }
    const double denominator = audio.frames ? double(audio.frames) : 1.0;
    QJsonObject result{{"schema", 1}, {"completed", completed}, {"passed", passed}, {"error", error},
        {"utcStart", utcStart}, {"utcEnd", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"wallSeconds", timer.nsecsElapsed() / 1e9}, {"maximumWallSeconds", kMaximumWallMs / 1000},
        {"nativeNrsc5Version", QString::fromUtf8(libraryVersion ? libraryVersion : "unknown")},
        {"executable", *executable}, {"compressedSource", compressed},
        {"compressedSourceRelation", "caller-supplied archive identity; decompression is external"},
        {"rawInput", QJsonObject{{"path", inputInfo.canonicalFilePath()}, {"declaredBytes", inputInfo.size()},
            {"readBytes", number(inputBytes)}, {"readSha256", QString::fromLatin1(rawHash.result().toHex())},
            {"format", "cu8"}, {"complexRateHz", NRSC5_SAMPLE_RATE_CU8}, {"readComplexFrames", number(inputFrames)},
            {"durationSeconds", double(inputFrames) / NRSC5_SAMPLE_RATE_CU8}, {"rfFrequencyHz", QJsonValue(QJsonValue::Null)},
            {"geometryReferenceHz", geometryReferenceHz}, {"translationHz", 0}, {"unsignedConversion", "(byte - 127) / 128"}}},
        {"decoderInput", QJsonObject{{"complexRateHz", Adapter::kOutputRate}, {"submittedComplexFrames", number(feed.iqFrames)},
            {"adapterOutputFrames", number(adapter.outputFrames())}, {"nativeFeedCount", number(feed.calls)},
            {"maximumNativeFeedComplexFrames", number(feed.maxFeedFrames)}, {"maximumInputBlockComplexFrames", qint64(Adapter::kMaxInput)},
            {"eofPolicy", "no invented RF padding or decoder flush; converter staging may retain a bounded tail"}}},
        {"observedSync", feed.observedSync}, {"discoveredPrograms", programs},
        {"lastReception", receptionJson(decoder.reception())},
        {"lastSyncedReception", feed.observedSync ? QJsonValue(receptionJson(feed.lastSynced)) : QJsonValue(QJsonValue::Null)},
        {"lastAudioReception", audio.callbacks ? QJsonValue(receptionJson(feed.lastAudio)) : QJsonValue(QJsonValue::Null)},
        {"audio", QJsonObject{{"rateHz", NRSC5_SAMPLE_RATE_AUDIO}, {"channelCount", 2}, {"program", program},
            {"frames", number(audio.frames)}, {"seconds", double(audio.frames) / NRSC5_SAMPLE_RATE_AUDIO},
            {"callbacks", number(audio.callbacks)}, {"maximumCallbackFrames", number(audio.largestCallbackFrames)},
            {"nonzeroFrames", number(audio.nonzeroFrames)}, {"differingLeftRightFrames", number(audio.differingFrames)},
            {"leftRms", std::sqrt(audio.leftEnergy / denominator)}, {"rightRms", std::sqrt(audio.rightEnergy / denominator)},
            {"leftRightDifferenceRms", std::sqrt(audio.differenceEnergy / denominator)},
            {"leftPeak", audio.leftPeak}, {"rightPeak", audio.rightPeak},
            {"leftSha256", QString::fromLatin1(audio.leftHash.result().toHex())},
            {"rightSha256", QString::fromLatin1(audio.rightHash.result().toHex())}, {"hashFormat", "float32 little endian per channel"},
            {"sourceSession", number(kSession)}, {"sourceRevision", number(kRevision)}, {"lastAudioEpoch", number(audio.lastAudioEpoch)},
            {"retirements", number(audio.retirements)}, {"identityOrSampleFaults", number(audio.faults)}}},
        {"burstAndGap", QJsonObject{{"maximumPcmCallbacksPerInputBlock", number(maxBlockCallbacks)},
            {"maximumPcmFramesPerInputBlock", number(maxBlockPcmFrames)}, {"maximumPcmCallbacksPerNativeFeed", number(feed.maxCallbacks)},
            {"maximumPcmFramesPerNativeFeed", number(feed.maxPcmFrames)}, {"maximumDecoderIqFramesBetweenAudioCallbacks", number(audio.maxInputGap)},
            {"maximumSameEpochDecoderIqFramesBetweenAudioCallbacks", number(audio.maxSameEpochInputGap)},
            {"maximumSourceSecondsBetweenAudioCallbacks", double(audio.maxInputGap) / Adapter::kOutputRate},
            {"maximumSameEpochSourceSecondsBetweenAudioCallbacks", double(audio.maxSameEpochInputGap) / Adapter::kOutputRate},
            {"firstAudioAtSourceSeconds", audio.callbacks ? QJsonValue(double(audio.firstAudioInput) / Adapter::kOutputRate) : QJsonValue(QJsonValue::Null)},
            {"trailingSourceSecondsWithoutAudio", audio.callbacks ? QJsonValue(double(feed.iqFrames - audio.lastAudioInput) / Adapter::kOutputRate) : QJsonValue(QJsonValue::Null)},
            {"maximumOfflineWallMsBetweenAudioCallbacks", number(audio.maxWallGapMs)},
            {"sourceTimeBasis", "actual converted IQ count at native-feed end; resolution is maximumNativeFeedComplexFrames"}}},
        {"scope", "offline native decoder/adapter; no worker queue, speaker playout, listening, GUI, USB or live RF proof"}};
#ifdef AETHER_GIT_SHA
    result.insert("aetherBuildRevision", QStringLiteral(AETHER_GIT_SHA));
#else
    result.insert("aetherBuildRevision", QJsonValue(QJsonValue::Null));
#endif
    printJson(result);
    return passed ? 0 : 1;
}
