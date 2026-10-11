// Explicit offline, source-clock-paced diagnostic. No USB, socket, audio device,
// PCM export, settings or injected decoder. Not a default build or CTest.
// Usage: hd_fm_worker_recording_probe RAW_CU8 [--program 0..7]
//        [--compressed-source SOURCE.xz]
#include "core/backends/rtl/RtlReceivePipeline.h"
#include "core/HdFmReception.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThreadPool>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
// The pinned C API header does not declare C++ linkage guards.
extern "C" {
#include <nrsc5.h>
}
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#ifndef AETHER_ENABLE_NRSC5
#error "This diagnostic requires the opt-in native HD decoder."
#endif
namespace {
using Pipeline = AetherSDR::rtl::RtlReceivePipeline;
using Transaction = Pipeline::Transaction;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr qint64 kMaximumBytes = 256 * 1024 * 1024;
constexpr auto kMaximumWall = 120s;
constexpr std::size_t kInputFrames = 8192;
constexpr double kRate = NRSC5_SAMPLE_RATE_CU8;
constexpr Transaction::Token kToken{1, 1};
static_assert(NRSC5_SAMPLE_RATE_CU8 == 1488375 && NRSC5_SAMPLE_RATE_AUDIO == 44100);
std::uint64_t nowMs()
{ return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }
QJsonValue number(std::uint64_t value) { return QJsonValue(static_cast<qint64>(value)); }
double seconds(Clock::duration value) { return std::chrono::duration<double>(value).count(); }
std::optional<double> processCpuSeconds()
{
#ifdef Q_OS_WIN
    // The Microsoft CRT's clock() measures elapsed wall time, not CPU time.
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) { return {}; }
    const auto ticks = [](const FILETIME& value) {
        return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    };
    return (static_cast<double>(ticks(kernel)) + static_cast<double>(ticks(user))) / 10000000.0;
#else
    const std::clock_t value = std::clock();
    if (value == std::clock_t(-1)) { return {}; }
    return double(value) / CLOCKS_PER_SEC;
#endif
}
void output(const QJsonObject& result)
{
    const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Indented);
    std::fwrite(bytes.constData(), 1, bytes.size(), stdout); std::fflush(stdout);
}
int fail(const QString& message)
{ output({{"schema", 1}, {"completed", false}, {"passed", false}, {"error", message}}); return 2; }
// Bounds even an upstream feed/planner/join that fails to return. This owns no
// hardware or user state; exit 124 is a failed, incomplete diagnostic.
class Deadline final {
public:
    Deadline() : m_thread([this] {
        std::unique_lock lock(m_mutex);
        if (!m_condition.wait_for(lock, kMaximumWall, [this] { return m_done; })) {
            std::fputs("HD worker recording probe exceeded its 120 s hard process budget\n", stderr);
            std::fflush(stderr); std::_Exit(124);
        }
    }) {}
    ~Deadline()
    {
        { const std::lock_guard lock(m_mutex); m_done = true; }
        m_condition.notify_all(); m_thread.join();
    }
private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_done = false;
    std::thread m_thread;
};
std::optional<QJsonObject> fileIdentity(const QString& path, qint64 maximum)
{
    QFileInfo info(path); QFile file(path);
    if (!info.isFile() || info.size() <= 0 || info.size() > maximum || !file.open(QIODevice::ReadOnly)
        || file.isSequential()) { return {}; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, 65536> bytes{}; qint64 total = 0;
    while (total < info.size()) {
        const auto count = file.read(bytes.data(), std::min<qint64>(bytes.size(), info.size() - total));
        if (count <= 0) { return {}; }
        hash.addData(QByteArrayView(bytes.data(), count)); total += count;
    }
    if (!file.atEnd() || file.size() != info.size()) { return {}; }
    return QJsonObject{{"path", info.canonicalFilePath()}, {"bytes", total},
        {"sha256", QString::fromLatin1(hash.result().toHex())}};
}
QJsonObject diagnostics(const Pipeline::Diagnostics& value)
{
    return {{"observed", value.observed}, {"droppedPackets", number(value.droppedPackets)},
        {"mixerLateFrames", number(value.mixerLateFrames)}, {"mixerRejectedBlocks", number(value.mixerRejectedBlocks)},
        {"mixerConfigurationFailures", number(value.mixerConfigurationFailures)}, {"droppedTraceEvents", number(value.droppedTraceEvents)}};
}
struct Audio {
    std::uint64_t packets = 0, frames = 0, nonzero = 0, differing = 0, invalid = 0;
    std::uint64_t acceptedPackets = 0, acceptedFrames = 0, acceptedNonzero = 0, rejectedPackets = 0;
    std::uint64_t rejectedProducedPackets = 0, rejectedProducedFrames = 0;
    std::uint64_t epoch = 0, next = 0, sequenceGaps = 0, discontinuities = 0, retirements = 0;
    std::uint64_t maxSourceAgeMs = 0, expiredPackets = 0, intentionalZeroFrames = 0, unexpectedNonzeroSilence = 0;
    double leftEnergy = 0, rightEnergy = 0, differenceEnergy = 0, peak = 0;
    void add(const Pipeline::Packet& packet, bool accepted, std::uint64_t now)
    {
        ++packets; frames += packet.frames; discontinuities += packet.discontinuity;
        if (epoch && packet.audioEpoch != epoch) { ++retirements; }
        if (epoch == packet.audioEpoch && packet.firstSample != next) { ++sequenceGaps; }
        epoch = packet.audioEpoch; next = packet.firstSample + packet.frames;
        if (packet.producedMonotonicMs) {
            if (packet.producedMonotonicMs > now) { ++invalid; }
            else {
                maxSourceAgeMs = std::max(maxSourceAgeMs, now - packet.producedMonotonicMs);
                expiredPackets += now - packet.producedMonotonicMs > 500;
            }
        } else { intentionalZeroFrames += packet.frames; }
        if (accepted) { ++acceptedPackets; acceptedFrames += packet.frames; } else {
            ++rejectedPackets;
            if (packet.producedMonotonicMs) { ++rejectedProducedPackets; rejectedProducedFrames += packet.frames; }
        }
        for (std::size_t i = 0; i < packet.frames; ++i) {
            const double l = packet.samples[2 * i], r = packet.samples[2 * i + 1];
            if (!std::isfinite(l) || !std::isfinite(r) || std::abs(l) > 1.0 || std::abs(r) > 1.0) { ++invalid; continue; }
            const bool audibleSample = l != 0 || r != 0;
            nonzero += audibleSample; differing += l != r; acceptedNonzero += accepted && audibleSample;
            unexpectedNonzeroSilence += !packet.producedMonotonicMs && audibleSample;
            leftEnergy += l * l; rightEnergy += r * r; differenceEnergy += (l - r) * (l - r);
            peak = std::max({peak, std::abs(l), std::abs(r)});
        }
    }
    QJsonObject json(int rate) const
    {
        const double divisor = frames ? double(frames) : 1.0;
        return {{"rateHz", rate}, {"channelCount", 2}, {"packets", number(packets)}, {"frames", number(frames)},
            {"nonzeroFrames", number(nonzero)}, {"differingLeftRightFrames", number(differing)}, {"invalidSamplesOrTime", number(invalid)},
            {"acceptedPackets", number(acceptedPackets)}, {"acceptedFrames", number(acceptedFrames)},
            {"acceptedNonzeroFrames", number(acceptedNonzero)}, {"rejectedByOwnerFencePackets", number(rejectedPackets)},
            {"rejectedProducedPackets", number(rejectedProducedPackets)}, {"rejectedProducedFrames", number(rejectedProducedFrames)},
            {"sameEpochSequenceGaps", number(sequenceGaps)}, {"epochTransitions", number(retirements)},
            {"discontinuities", number(discontinuities)}, {"maximumSourceAgeMs", number(maxSourceAgeMs)},
            {"expiredPackets", number(expiredPackets)}, {"intentionalZeroFrames", number(intentionalZeroFrames)},
            {"unexpectedNonzeroSilenceFrames", number(unexpectedNonzeroSilence)},
            {"leftRms", std::sqrt(leftEnergy / divisor)}, {"rightRms", std::sqrt(rightEnergy / divisor)},
            {"leftRightDifferenceRms", std::sqrt(differenceEnergy / divisor)}, {"peak", peak}};
    }
};
struct Observations {
    explicit Observations(int program) : expectedProgram(program) {}
    const int expectedProgram;
    Pipeline::HdFmObservation last;
    Audio tap, speaker;
    QJsonArray transitions, traces;
    std::uint64_t observationCount = 0, invalid = 0, expiredObservations = 0, counterRegressions = 0;
    std::uint64_t iqDrops = 0, pcmDrops = 0, underruns = 0, publication = 0;
    std::uint64_t transitionOverflow = 0, traceCount = 0, traceOverflow = 0;
    double firstReady = -1, readyStart = 0, longestReady = 0;
    bool ready = false, hadObservation = false;
    unsigned discovered = 0;
    bool fresh(std::uint64_t stamp, std::uint64_t now) const { return stamp && stamp <= now && now - stamp <= 500; }
    bool audioReady(std::uint64_t now) const
    {
        const auto& raw = last.reception;
        return hadObservation && raw.valid && raw.synced && raw.audioValid
            && fresh(raw.observationMonotonicMs, now) && fresh(raw.audioMonotonicMs, now);
    }
    void drain(Pipeline& pipeline, std::uint64_t sourceFrames)
    {
        const auto now = nowMs(); const double sourceSeconds = sourceFrames / kRate;
        Pipeline::HdFmObservation observation;
        while (pipeline.takeHdObservation(observation)) {
            ++observationCount; const auto& raw = observation.reception;
            if (observation.token != kToken || observation.slot != 0 || !observation.instance
                || !observation.receiverEpoch || !observation.captureEpoch || !observation.audioEpoch
                || raw.sessionId != kToken.session || raw.revision != kToken.revision
                || raw.receiverEpoch != observation.receiverEpoch || raw.audioEpoch != observation.audioEpoch
                || raw.selectedProgram != expectedProgram || raw.frequencyHz != 1000000
                || raw.publicationSequence <= publication) { ++invalid; continue; }
            publication = raw.publicationSequence;
            counterRegressions += raw.iqDrops < iqDrops || raw.pcmDrops < pcmDrops || raw.playoutUnderruns < underruns;
            iqDrops = std::max(iqDrops, raw.iqDrops); pcmDrops = std::max(pcmDrops, raw.pcmDrops);
            underruns = std::max(underruns, raw.playoutUnderruns);
            for (const auto& service : raw.services) {
                if (service.program >= 0 && service.program < 8) { discovered |= 1u << service.program; }
            }
            if (raw.valid && !fresh(raw.observationMonotonicMs, now)) { ++expiredObservations; continue; }
            last = observation; hadObservation = true;
        }
        const bool currentReady = audioReady(now);
        if (currentReady != ready) {
            if (currentReady) { readyStart = sourceSeconds; if (firstReady < 0) { firstReady = sourceSeconds; } }
            else { longestReady = std::max(longestReady, sourceSeconds - readyStart); }
            if (transitions.size() < 512) {
                transitions.append(QJsonObject{{"sourceSeconds", sourceSeconds}, {"monotonicMs", number(now)},
                    {"ready", currentReady}, {"audioEpoch", number(last.audioEpoch)},
                    {"publicationSequence", number(last.reception.publicationSequence)},
                    {"audioSequence", number(last.reception.audioSequence)},
                    {"synced", last.reception.synced}, {"valid", last.reception.valid}});
            } else { ++transitionOverflow; }
            ready = currentReady;
        }
        Pipeline::Packet packet;
        while (pipeline.takePacket(packet)) {
            if (packet.token != kToken || packet.frames == 0 || packet.frames > 1024 || packet.channelCount != 2
                || (packet.slot != 0 && packet.slot != -1) || (packet.slot == 0 ? packet.sampleRateHz != 44100 : packet.sampleRateHz != 48000)) {
                ++invalid; continue;
            }
            const bool identity = hadObservation && packet.audioEpoch == last.audioEpoch && packet.captureEpoch == last.captureEpoch;
            bool accepted = identity;
            if (packet.slot == 0) {
                accepted &= currentReady && fresh(packet.producedMonotonicMs, now)
                    && packet.instance == last.instance && packet.receiverEpoch == last.receiverEpoch;
                tap.add(packet, accepted, now);
            } else {
                accepted &= last.reception.valid && fresh(last.reception.observationMonotonicMs, now)
                    && (packet.producedMonotonicMs ? currentReady && fresh(packet.producedMonotonicMs, now) : !currentReady);
                speaker.add(packet, accepted, now);
            }
        }
        Pipeline::TraceEvent trace;
        while (pipeline.takeTraceEvent(trace)) {
            ++traceCount;
            if (traces.size() < 128) {
                traces.append(QJsonObject{{"kind", trace.kind == Pipeline::TraceEvent::Kind::ReceiverFailure ? "receiver_failure" : "mixer_missing"},
                    {"captureFirst", number(trace.captureFirst)}, {"captureFrames", number(trace.captureFrames)},
                    {"quantumFirst", number(trace.quantumFirst)}, {"receiverEpoch", number(trace.receiverEpoch)},
                    {"missingLow", QString::number(trace.missingMask[0], 16)}, {"missingHigh", QString::number(trace.missingMask[1], 16)}});
            } else { ++traceOverflow; }
        }
    }
};
double percentile(std::vector<double> values, double fraction)
{
    if (values.empty()) { return 0; }
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, static_cast<std::size_t>(std::ceil(fraction * values.size())) - 1)];
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv); Deadline hardDeadline;
    const auto start = Clock::now(); const auto arguments = app.arguments();
    if (arguments.size() < 2 || arguments.size() > 6) { return fail(QStringLiteral("Usage: hd_fm_worker_recording_probe RAW_CU8 [--program 0..7] [--compressed-source SOURCE.xz]")); }
    int program = 0; bool programSeen = false; QString compressedPath;
    for (qsizetype i = 2; i < arguments.size(); i += 2) {
        if (i + 1 >= arguments.size()) { return fail(QStringLiteral("Missing option value")); }
        if (arguments[i] == QLatin1String("--program") && !programSeen) {
            bool ok = false; program = arguments[i + 1].toInt(&ok); programSeen = true;
            if (!ok || program < 0 || program > 7) { return fail(QStringLiteral("Program must be 0..7")); }
        } else if (arguments[i] == QLatin1String("--compressed-source") && compressedPath.isEmpty()) { compressedPath = arguments[i + 1]; }
        else { return fail(QStringLiteral("Unknown or repeated option")); }
    }
    QFileInfo inputInfo(arguments[1]); QFile input(arguments[1]);
    if (!inputInfo.isFile() || inputInfo.size() <= 0 || inputInfo.size() > kMaximumBytes || inputInfo.size() % 2
        || !input.open(QIODevice::ReadOnly) || input.isSequential()) { return fail(QStringLiteral("Input must be regular even-sized decompressed CU8, at most 256 MiB")); }
    if (input.peek(6) == QByteArray::fromHex("fd377a585a00")) { return fail(QStringLiteral("Decompress the recording before running")); }
    const auto executable = fileIdentity(QCoreApplication::applicationFilePath(), 1024LL * 1024 * 1024);
    if (!executable) { return fail(QStringLiteral("Cannot hash running executable")); }
    QJsonValue compressed(QJsonValue::Null);
    if (!compressedPath.isEmpty()) {
        const auto value = fileIdentity(compressedPath, kMaximumBytes);
        if (!value) { return fail(QStringLiteral("Cannot hash bounded compressed source")); }
        compressed = *value;
    }
    Transaction::State state; state.token = kToken;
    state.hardware.centerHz = 1000000; state.hardware.sampleRateHz = NRSC5_SAMPLE_RATE_CU8;
    state.capture = {1, 1, 1000000, kRate, 0.45 * kRate, 0.45 * kRate};
    Transaction::Receiver receiver; receiver.passband = {0, 1000000, -100000, 100000, 0, 3000, 3000};
    receiver.mode = Transaction::Mode::Wfm; receiver.wfmHdStereo = true; receiver.hdProgram = program;
    state.receivers = {receiver}; state.receivingIds = {0};
    auto pipeline = std::make_unique<Pipeline>(1, true);
    const auto prepareStart = Clock::now();
    const auto preparationFailure = [&](const QString& message) {
        pipeline->stop(); pipeline.reset();
        const bool released = QThreadPool::globalInstance()->waitForDone(9000);
        const int code = fail(message + (released ? QString() : QStringLiteral("; teardown also timed out")));
        if (!released) { std::_Exit(code); }
        return code;
    };
    if (!pipeline->prepare(state, true)) { return preparationFailure(QStringLiteral("Production pipeline refused HD preparation")); }
    bool prepared = false;
    while (Clock::now() - prepareStart < 20s) {
        const auto result = pipeline->service();
        if (result == Pipeline::Preparation::Failed) { break; }
        if (result == Pipeline::Preparation::Ready) { prepared = pipeline->adopt(); break; }
        std::this_thread::sleep_for(1ms);
    }
    if (!prepared) { return preparationFailure(QStringLiteral("Native HD preparation/adoption failed or exceeded 20 s")); }
    const double preparationSeconds = seconds(Clock::now() - prepareStart);
    const auto initialDiagnostics = pipeline->diagnostics();
    QCryptographicHash rawHash(QCryptographicHash::Sha256);
    std::array<char, 2 * kInputFrames> bytes{};
    std::array<std::complex<float>, kInputFrames> samples{};
    std::vector<double> callbackLatenessMs, processUs; callbackLatenessMs.reserve(16384); processUs.reserve(16384);
    Observations observations(program); QString error;
    std::uint64_t inputBytes = 0, inputFrames = 0, calls = 0, catchup = 0, maxCatchup = 0;
    const auto playbackStart = Clock::now(); const auto cpuStart = processCpuSeconds();
    const auto utcStart = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    while (inputBytes < static_cast<std::uint64_t>(inputInfo.size())) {
        if (Clock::now() - start > 110s) { error = QStringLiteral("Playback reached soft wall budget; reserving teardown time"); break; }
        const auto count = input.read(bytes.data(), std::min<qint64>(bytes.size(), inputInfo.size() - static_cast<qint64>(inputBytes)));
        if (count <= 0 || count % 2) { error = QStringLiteral("Read error or truncated IQ pair"); break; }
        rawHash.addData(QByteArrayView(bytes.data(), count)); const std::size_t frames = count / 2;
        for (std::size_t i = 0; i < frames; ++i) {
            samples[i] = {(static_cast<unsigned char>(bytes[2 * i]) - 127.0f) / 128.0f,
                (static_cast<unsigned char>(bytes[2 * i + 1]) - 127.0f) / 128.0f};
        }
        const auto target = playbackStart + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>((inputFrames + frames) / kRate));
        if (Clock::now() < target) { std::this_thread::sleep_until(target); catchup = 0; }
        else { maxCatchup = std::max(maxCatchup, ++catchup); }
        const auto entered = Clock::now(); callbackLatenessMs.push_back(1000 * seconds(entered - target));
        const bool accepted = pipeline->process(inputFrames, std::span(samples).first(frames));
        processUs.push_back(1e6 * seconds(Clock::now() - entered));
        inputBytes += count; inputFrames += frames; ++calls;
        observations.drain(*pipeline, inputFrames);
        if (!accepted || pipeline->needsRepair()) { error = QStringLiteral("Production receiver/pipeline withdrew; no repair or retry is hidden"); break; }
    }
    const auto cpuEnd = processCpuSeconds(); const double playbackWallSeconds = seconds(Clock::now() - playbackStart);
    observations.drain(*pipeline, inputFrames);
    if (observations.ready) { observations.longestReady = std::max(observations.longestReady, inputFrames / kRate - observations.readyStart); }
    const auto finalDiagnostics = pipeline->diagnostics(); const bool repair = pipeline->needsRepair();
    const auto teardownStart = Clock::now(); pipeline->stop(); pipeline.reset();
    const bool teardownComplete = QThreadPool::globalInstance()->waitForDone(9000);
    const double teardownSeconds = seconds(Clock::now() - teardownStart);
    if (!teardownComplete && error.isEmpty()) { error = QStringLiteral("Registry/decoder teardown did not complete within 9 s"); }
    const bool completed = error.isEmpty() && inputBytes == static_cast<std::uint64_t>(inputInfo.size())
        && input.atEnd() && input.size() == inputInfo.size() && teardownComplete;
    if (!completed && error.isEmpty()) { error = QStringLiteral("Input changed or ended early"); }
    const bool zeroPipelineFaults = !finalDiagnostics.droppedPackets && !finalDiagnostics.mixerLateFrames
        && !finalDiagnostics.mixerRejectedBlocks && !finalDiagnostics.mixerConfigurationFailures && !finalDiagnostics.droppedTraceEvents;
    const bool passed = completed && !repair && zeroPipelineFaults && !observations.invalid && !observations.counterRegressions && !observations.expiredObservations
        && !observations.iqDrops && !observations.pcmDrops && !observations.underruns
        && !observations.tap.invalid && !observations.speaker.invalid && !observations.speaker.unexpectedNonzeroSilence
        && !observations.tap.sequenceGaps && !observations.speaker.sequenceGaps
        && !observations.tap.expiredPackets && !observations.speaker.expiredPackets
        && !observations.tap.rejectedProducedPackets && !observations.speaker.rejectedProducedPackets
        && !observations.tap.intentionalZeroFrames
        && observations.tap.acceptedFrames && observations.speaker.acceptedNonzero;
    QJsonArray programs; for (int i = 0; i < 8; ++i) { if (observations.discovered & (1u << i)) { programs.append(i); } }
    const auto& raw = observations.last.reception;
    QJsonObject result{{"schema", 1}, {"completed", completed}, {"passed", passed}, {"error", error},
        {"utcStart", utcStart}, {"utcEnd", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"maximumWallSeconds", 120}, {"totalWallSeconds", seconds(Clock::now() - start)},
        {"preparationSeconds", preparationSeconds}, {"playbackWallSeconds", playbackWallSeconds},
        {"teardownSeconds", teardownSeconds}, {"teardownComplete", teardownComplete}, {"executable", *executable},
        {"compressedSource", compressed}, {"compressedSourceRelation", "caller-supplied archive identity; decompression external"},
        {"rawInput", QJsonObject{{"path", inputInfo.canonicalFilePath()}, {"declaredBytes", inputInfo.size()}, {"readBytes", number(inputBytes)},
            {"readSha256", QString::fromLatin1(rawHash.result().toHex())}, {"format", "cu8"}, {"complexRateHz", kRate},
            {"readComplexFrames", number(inputFrames)}, {"durationSeconds", inputFrames / kRate}, {"program", program},
            {"rfFrequencyHz", QJsonValue(QJsonValue::Null)}, {"geometryReferenceHz", 1000000}, {"unsignedConversion", "(byte - 127) / 128"}}},
        {"pacing", QJsonObject{{"rateMultiplier", 1}, {"blockFramesMaximum", qint64(kInputFrames)}, {"callbacks", number(calls)},
            {"latenessP95Ms", percentile(callbackLatenessMs, 0.95)}, {"latenessMaximumMs", percentile(callbackLatenessMs, 1)},
            {"processP95Us", percentile(processUs, 0.95)}, {"processMaximumUs", percentile(processUs, 1)},
            {"maximumConsecutiveCatchupCallbacks", number(maxCatchup)}, {"policy", "absolute source-block-end deadlines; lateness is retained, no rebasing"}}},
        {"nativeTap", observations.tap.json(44100)}, {"speaker", observations.speaker.json(48000)},
        {"initialDiagnostics", diagnostics(initialDiagnostics)}, {"finalDiagnostics", diagnostics(finalDiagnostics)}, {"needsRepair", repair},
        {"hd", QJsonObject{{"observations", number(observations.observationCount)}, {"invalidIdentity", number(observations.invalid)},
            {"expiredObservations", number(observations.expiredObservations)}, {"counterRegressions", number(observations.counterRegressions)},
            {"iqDrops", number(observations.iqDrops)}, {"pcmDrops", number(observations.pcmDrops)}, {"playoutUnderruns", number(observations.underruns)},
            {"firstReadyAtSourceSeconds", observations.firstReady < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(observations.firstReady)},
            {"longestReadySourceSeconds", observations.longestReady}, {"finalRawSynced", raw.synced}, {"finalRawAudioValid", raw.audioValid},
            {"finalAudioEpoch", number(raw.audioEpoch)}, {"syncLossCount", number(raw.syncLossCount)}, {"reacquisitionCount", number(raw.reacquisitionCount)},
            {"discoveredPrograms", programs}, {"transitions", observations.transitions}, {"omittedTransitions", number(observations.transitionOverflow)}}},
        {"traceCount", number(observations.traceCount)}, {"traces", observations.traces}, {"omittedTraces", number(observations.traceOverflow)},
        {"scope", "real pipeline, registry, native worker and shared mixer; owner-like identity/age analysis, not full backend normalization/high-water acceptance or GUI/audio-device/USB/live RF proof"},
        {"queueOccupancyMeasured", false},
        {"eofPolicy", "no RF padding or flush; stop discards pending worker/converter/playout tail; totals describe delivered prefix only"}};
    if (cpuStart && cpuEnd && *cpuEnd >= *cpuStart) {
        result.insert("playbackProcessCpuSeconds", *cpuEnd - *cpuStart);
    }
#ifdef AETHER_GIT_SHA
    result.insert("aetherBuildRevision", QStringLiteral(AETHER_GIT_SHA));
#else
    result.insert("aetherBuildRevision", QJsonValue(QJsonValue::Null));
#endif
    output(result);
    // A failed join cannot fall into an unbounded Qt global-pool destructor.
    // This standalone diagnostic owns no device or external mutable state.
    if (!teardownComplete) { std::_Exit(2); }
    return passed ? 0 : 1;
}
