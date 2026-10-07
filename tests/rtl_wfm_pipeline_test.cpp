#include "core/backends/rtl/RtlReceivePipeline.h"
#include "core/backends/rtl/RtlRfExtractor.h"
#include "core/backends/rtl/RtlSdrDdc.h"
#include "core/dsp/WdspChannel.h"

#include <QCoreApplication>
#include <QThreadPool>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <numbers>
#include <numeric>
#include <thread>
#include <vector>

using Pipeline = AetherSDR::rtl::RtlReceivePipeline;
using Extractor = AetherSDR::rtl::RtlRfExtractor;
using Transaction = AetherSDR::rtl::RtlCaptureTransaction;
using namespace std::chrono_literals;
namespace {
constexpr double kTau = 2 * std::numbers::pi;
int failures = 0;
void check(bool condition, const char* description)
{
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", description); }
}
bool ready(Pipeline& pipeline)
{
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto result = pipeline.service();
        if (result == Pipeline::Preparation::Ready) { return true; }
        if (result == Pipeline::Preparation::Failed) { return false; }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}
Transaction::State state()
{
    Transaction::State result;
    result.token = {47, 1};
    result.hardware.centerHz = 100'000'000;
    result.capture = {47, 1, 100'000'000, 2'400'000, 1'080'000, 1'080'000};
    result.receivers = {{{3, 100'000'000, -100'000, 100'000, 0, 3000, 3000}, Transaction::Mode::Wfm}};
    result.receivingIds = {3};
    return result;
}
class ClockSink final : public Extractor::Sink {
public:
    bool iqBlock(std::span<const float> i, std::span<const float> q,
                 std::uint64_t position) noexcept override
    {
        check(i.size() == 2048 && q.size() == i.size(), "WFM extracted IQ uses complete paired blocks");
        check(position % 8 == 0, "384 kHz IQ starts on the final 48 kHz sample lattice");
        if (blocks == 0) { first = position; }
        else { check(position == next, "extracted IQ positions advance without partition-dependent rounding"); }
        next = position + i.size();
        ++blocks;
        return true;
    }
    std::uint64_t first = 0;
    std::uint64_t next = 0;
    unsigned blocks = 0;
};
void outputClockAlignment()
{
    // At 2.4 MS/s, capture sample 25 is on the 384 kHz lattice but halfway
    // between final 48 kHz frames. Coprime rates also exercise the bounded wait.
    for (std::uint64_t rate : {225001u, 1843200u, 2400000u, 2400001u}) {
        const std::uint64_t first = 25;
        const std::uint64_t period = rate / std::gcd(rate, std::uint64_t{48000});
        const std::uint64_t start = first + (period - first % period) % period;
        Extractor::Config config{{1, 1, 100'000'000, double(rate), rate * 0.45, rate * 0.45},
            {3, 100'000'000, -100'000, 100'000, 0, 1000, 1000}, 384000, 2048, 48000};
        Extractor extractor(config);
        check(extractor.valid(), "WFM extraction accepts achieved unusual rates and bounded interpolation");
        if (!extractor.valid()) { continue; }
        ClockSink sink;
        std::array<std::complex<float>, 257> input;
        input.fill({0.3f, 0});
        const std::uint64_t end = start + rate / 4;
        for (std::uint64_t position = first; position < end;) {
            const std::size_t count = std::min<std::uint64_t>(input.size(), end - position);
            check(extractor.process(config.capture, position, std::span(input).first(count), sink),
                  "partitioned input preserves the capture sample clock");
            position += count;
        }
        check(sink.blocks > 4, "clock fixture observes actual paired resampler output");
        check(sink.first == start / period * (384000 / std::gcd(rate, std::uint64_t{48000})),
              "first IQ position is the exact capture-derived origin");
        check(sink.first / 8 == start / period * (48000 / std::gcd(rate, std::uint64_t{48000})),
              "audio origin converts exactly to the shared 48 kHz timeline");
        check(!extractor.process(config.capture, end + 1, input, sink),
              "a capture gap withdraws the extractor instead of relabeling old history");
    }
}

struct Audio {
    std::vector<float> left;
    std::vector<float> right;
    std::uint64_t next = 0;
    std::uint64_t instance = 0;
    std::uint64_t receiverEpoch = 0;
    std::uint64_t captureEpoch = 0;
    bool seen = false;
    bool stereo = false;
    bool pilot = false;
    bool discontinuity = false;
    double peak = 0;
    std::size_t clipped = 0;
    void accept(const Pipeline::Packet& packet)
    {
        check(packet.sampleRateHz == 48000 && packet.channelCount == 2,
              "native audio declares 48 kHz paired stereo");
        check(packet.frames == (packet.slot < 0 ? 128u : 256u),
              "speaker and slice packet frame counts describe their actual output blocks");
        if (seen) { check(packet.firstSample == next, "native output positions are contiguous at 48 kHz"); }
        else {
            instance = packet.instance;
            receiverEpoch = packet.receiverEpoch;
            captureEpoch = packet.captureEpoch;
            discontinuity = packet.discontinuity;
        }
        seen = true;
        next = packet.firstSample + packet.frames;
        stereo |= packet.wfmStereoDetected.value_or(false);
        pilot |= packet.wfmReception && packet.wfmReception->pilotLocked;
        for (std::size_t i = 0; i < packet.frames; ++i) {
            const float l = packet.samples[2 * i];
            const float r = packet.samples[2 * i + 1];
            check(std::isfinite(l) && std::isfinite(r), "native WFM samples remain finite");
            left.push_back(l); right.push_back(r);
            peak = std::max({peak, std::abs(double(l)), std::abs(double(r))});
            clipped += std::abs(l) >= 0.999999f || std::abs(r) >= 0.999999f;
        }
    }
};
// Integrate the published broadcast multiplex analytically. No firmware peer:
// L=.35*sin(1k), R=.20*sin(2k), pilot=.1*sin(19k), difference at sin(38k).
// Pilot/subcarrier phase follows the same public broadcast reference as the
// low-level WDSP fixture; changing only one to cosine would test quadrature.
std::complex<float> referenceIq(std::uint64_t position, double rate)
{
    const double time = position / rate;
    const auto sineIntegral = [time](double hz) { return -std::cos(kTau * hz * time) / hz; };
    const auto cosineIntegral = [time](double hz) { return std::sin(kTau * hz * time) / hz; };
    const double phase = 75000 * (0.1575 * sineIntegral(1000) + 0.09 * sineIntegral(2000)
        + 0.07875 * (cosineIntegral(37000) - cosineIntegral(39000))
        - 0.045 * (cosineIntegral(36000) - cosineIntegral(40000)) + 0.1 * sineIntegral(19000));
    return {float(0.3 * std::cos(phase)), float(0.3 * std::sin(phase))};
}
std::array<Audio, 2> run(Pipeline& pipeline, const Transaction::State& current,
                         std::uint64_t& position, double seconds)
{
    std::array<Audio, 2> result;
    for (Audio& audio : result) {
        audio.left.reserve(std::size_t(seconds * 48000) + 1024);
        audio.right.reserve(std::size_t(seconds * 48000) + 1024);
    }
    const auto started = std::chrono::steady_clock::now();
    const std::uint64_t origin = position;
    const double rate = current.capture.achievedSampleRateHz;
    const std::uint64_t end = position + std::uint64_t(seconds * rate);
    std::array<std::complex<float>, 8192> iq;
    while (position < end) {
        const std::size_t count = std::min<std::uint64_t>(iq.size(), end - position);
        for (std::size_t i = 0; i < count; ++i) { iq[i] = referenceIq(position + i, rate); }
        if (!pipeline.process(position, std::span(iq).first(count))) {
            check(false, "WFM acquisition accepts continuous generated IQ"); break;
        }
        position += count;
        Pipeline::Packet packet;
        while (pipeline.takePacket(packet)) {
            check(packet.token == current.token, "PCM and decoder status retain the accepted capture token");
            check(packet.slot == -1 || packet.slot == 3, "native routing preserves sparse slice identity");
            if (packet.slot < 0) {
                check(!packet.wfmStereoDetected && !packet.wfmReception,
                      "speaker mix does not claim an independent receiver observation");
            } else {
                // Prefilled D8 PCM can precede the first completed decoder
                // observation. Absence is truthful; status must not be invented.
                check(packet.wfmStereoDetected.has_value() == packet.wfmReception.has_value(),
                      "slice output status is published only with a decoder observation");
                if (packet.wfmReception) {
                    check(packet.wfmReception->valid && packet.wfmStereoDetected
                              == (packet.wfmReception->pilotLocked && !current.receivers[0].wfmForceMono),
                          "observed output status honors the accepted mono selection and independent pilot");
                }
            }
            result[packet.slot < 0 ? 1 : 0].accept(packet);
        }
        std::this_thread::sleep_until(started + std::chrono::microseconds(
            std::uint64_t((position - origin) * 1'000'000.0 / rate)));
    }
    check(!pipeline.needsRepair(), "real WDSP processing does not withdraw the receiver");
    return result;
}
double amplitude(const std::vector<float>& samples, double hz)
{
    constexpr std::size_t window = 48000;
    if (samples.size() < window) { return 0; }
    std::complex<double> sum{};
    const std::size_t first = samples.size() - window;
    for (std::size_t i = 0; i < window; ++i) {
        const double phase = -kTau * hz * i / 48000;
        sum += double(samples[first + i]) * std::complex<double>(std::cos(phase), std::sin(phase));
    }
    return 2 * std::abs(sum) / window;
}
void nativeRouting()
{
    auto pipeline = std::make_unique<Pipeline>(1, true);
    Transaction::State current = state();
    check(pipeline->prepare(current, true) && ready(*pipeline) && pipeline->adopt(),
          "qualified WFM prepares and adopts the production native graph");
    if (pipeline->legacy()) { pipeline->stop(); return; }
    std::uint64_t position = 25;
    const auto output = run(*pipeline, current, position, 4.0);
    for (const Audio& audio : output) {
        const double left = amplitude(audio.left, 1000);
        const double leftLeak = amplitude(audio.left, 2000);
        const double right = amplitude(audio.right, 2000);
        const double rightLeak = amplitude(audio.right, 1000);
        std::printf("WFM_PIPELINE samples=%zu peak=%.6f L1k=%.6f L2k=%.6f R2k=%.6f R1k=%.6f clipped=%zu\n",
            audio.left.size(), audio.peak, left, leftLeak, right, rightLeak, audio.clipped);
        check(audio.left.size() > 3 * 48000 && audio.left.size() <= 4 * 48000,
              "native duration follows the 48 kHz output clock");
        check(audio.discontinuity, "a newly installed WFM graph starts discontinuously");
        check(left > 0.001 && right > 0.001, "both reference stereo channels reach native output");
        check(left > 100 * leftLeak && right > 100 * rightLeak,
              "native WFM preserves correct L/R with at least 40 dB separation");
        check(audio.peak < 1 && audio.clipped == 0, "normalization precedes the independent tap and final speaker limiter");
    }
    check(output[0].stereo, "actual WDSP pilot acquisition reaches slice metadata");

    current.token.revision++;
    current.receivers[0].audioMute = true;
    check(pipeline->prepare(current) && ready(*pipeline) && pipeline->adopt(), "monitor mute adopts without replacing the receiver");
    const auto muted = run(*pipeline, current, position, 0.5);
    check(muted[0].peak > 0.001 && muted[1].peak == 0, "monitor mute silences speakers while the independent tap remains live");
    check(muted[0].receiverEpoch == output[0].receiverEpoch, "monitor edits retain the decoder epoch");

    current.token.revision++;
    current.receivers[0].audioMute = false;
    current.receivers[0].wfmDeemphasisUs = 50;
    check(pipeline->prepare(current) && ready(*pipeline) && pipeline->adopt(), "50 us deemphasis prepares a fresh immutable decoder recipe");
    const auto changed = run(*pipeline, current, position, 1.0);
    check(changed[0].seen && changed[1].seen && changed[0].discontinuity && changed[1].discontinuity,
          "deemphasis changes mark both slice and speaker discontinuities");
    check(changed[0].receiverEpoch != output[0].receiverEpoch && changed[1].captureEpoch != output[1].captureEpoch,
          "deemphasis replaces both decoder and speaker epochs");

    current.token.revision++;
    current.receivers[0].wfmForceMono = true;
    check(pipeline->prepare(current) && ready(*pipeline) && pipeline->adopt(), "Force Mono prepares an accepted decoder recipe");
    const auto mono = run(*pipeline, current, position, 4.0);
    for (const Audio& audio : mono) {
        check(audio.seen && audio.peak > 0.001 && audio.left == audio.right,
              "Force Mono is identical nonzero L/R at independent tap and speaker");
        check(audio.discontinuity, "Force Mono retires both audio lifetimes");
    }
    check(!mono[0].stereo && mono[0].pilot && mono[0].receiverEpoch != changed[0].receiverEpoch
        && mono[1].captureEpoch != changed[1].captureEpoch,
        "Force Mono reports mono output with real pilot detection while replacing decoder and speaker epochs");
    current.token.revision++;
    current.receivers[0].wfmForceMono = false;
    check(pipeline->prepare(current) && ready(*pipeline) && pipeline->adopt(), "Auto Stereo readopts");
    const auto automatic = run(*pipeline, current, position, 4.0);
    check(amplitude(automatic[0].left, 1000) > 100 * amplitude(automatic[0].right, 1000)
        && amplitude(automatic[1].right, 2000) > 100 * amplitude(automatic[1].left, 2000),
        "Auto Stereo restores separated native tap and speaker audio");
    current.token.revision++;
    current.receivingIds.clear();
    check(pipeline->prepare(current) && ready(*pipeline) && pipeline->adopt(), "parking admits an empty active bank");
    const auto parked = run(*pipeline, current, position, 0.05);
    check(!parked[0].seen && !parked[1].seen, "parked WFM emits neither stale audio nor pilot observations");
    current.token.revision++;
    current.receivingIds = {3};
    Pipeline::Submission resume = Pipeline::Submission::RetryRetiringSlot;
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while ((resume == Pipeline::Submission::RetryRetiringSlot
            || resume == Pipeline::Submission::RetryPlannerBusy)
           && std::chrono::steady_clock::now() < deadline) {
        resume = pipeline->prepareDetailed(current);
        if (resume != Pipeline::Submission::Accepted) {
            pipeline->service();
            std::this_thread::sleep_for(1ms);
        }
    }
    check(resume == Pipeline::Submission::Accepted && ready(*pipeline) && pipeline->adopt(),
          "resuming waits for safe retirement before constructing a fresh WFM receiver");
    const auto resumed = run(*pipeline, current, position, 1.5);
    check(resumed[0].seen && resumed[0].instance != changed[0].instance
              && resumed[0].discontinuity && resumed[1].discontinuity,
          "resumed WFM cannot inherit the retired receiver's PCM or pilot lifetime");
    // A gap withdraws the immutable receiver. Repair has the same recipe,
    // but a fresh decoder/filter history must also retire the speaker history.
    std::array<std::complex<float>, 257> gapIq;
    ++position;
    for (std::size_t i = 0; i < gapIq.size(); ++i) {
        gapIq[i] = referenceIq(position + i, current.capture.achievedSampleRateHz);
    }
    check(pipeline->process(position, gapIq) && pipeline->needsRepair(),
          "capture gaps withdraw WFM and request the existing owner repair path");
    position += gapIq.size();
    Pipeline::Packet stale;
    while (pipeline->takePacket(stale)) {} // consume only the old accepted revision
    current.token.revision++;
    check(pipeline->prepare(current) && ready(*pipeline) && pipeline->adopt(),
          "an unchanged WFM recipe can repair its withdrawn decoder");
    const auto repaired = run(*pipeline, current, position, 1.0);
    check(repaired[0].seen && repaired[1].seen
              && repaired[0].receiverEpoch != resumed[0].receiverEpoch
              && repaired[1].captureEpoch != resumed[1].captureEpoch
              && repaired[0].discontinuity && repaired[1].discontinuity,
          "WFM repair retires both decoder and speaker histories");
    const auto diagnostics = pipeline->diagnostics();
    check(diagnostics.droppedPackets == 0 && diagnostics.mixerRejectedBlocks == 0
              && diagnostics.mixerConfigurationFailures == 0,
          "paced WFM integration keeps bounded queues and valid mixer positions");
    pipeline->stop();
}
void workerUnderrunTrace()
{
    using Registry = AetherSDR::rtl::RtlReceiverRegistry;
    auto pipeline = std::make_unique<Pipeline>(1, true);
    Transaction::State current = state();
    const bool installed = pipeline->prepare(current, true) && ready(*pipeline) && pipeline->adopt();
    check(installed && !pipeline->legacy(), "underrun fixture prepares the actual nonblocking WFM receiver");
    if (!installed || pipeline->legacy()) { pipeline->stop(); return; }

    // The first worker to reach the existing handoff point is the sole WFM
    // worker in this fixture. Destruction order releases it before the pipeline
    // can join/retire WDSP, including every failed-assertion/early-return path.
    struct HandoffHold {
        HandoffHold() { WdspChannel::setWorkerHandoffHoldForTest(true); }
        ~HandoffHold() { release(); }
        void release() { WdspChannel::setWorkerHandoffHoldForTest(false); }
    } hold;
    std::uint64_t position = 0;
    std::uint64_t failedCaptureFirst = 0;
    std::size_t slicePackets = 0;
    Pipeline::Packet firstSlice;
    std::array<std::complex<float>, 256> iq;
    const auto feed = [&] {
        for (std::size_t i = 0; i < iq.size(); ++i) {
            iq[i] = referenceIq(position + i, current.capture.achievedSampleRateHz);
        }
        check(pipeline->process(position, iq), "continuous capture remains admitted while WDSP is held");
        if (pipeline->needsRepair()) { failedCaptureFirst = position; }
        position += iq.size();
        Pipeline::Packet packet;
        while (pipeline->takePacket(packet)) {
            if (packet.slot == 3) {
                if (slicePackets == 0) { firstSlice = packet; }
                check(packet.firstSample == slicePackets * 256,
                      "successful pre-fault slice PCM retains its exact 48 kHz positions");
                ++slicePackets;
            }
        }
    };
    // Small capture partitions produce at most one WDSP block per call. Stop
    // after the first output, then wait for an explicit worker acknowledgement;
    // no elapsed sleep is assumed to imply that the worker has been scheduled.
    for (unsigned chunk = 0; chunk < 512 && slicePackets == 0 && !pipeline->needsRepair(); ++chunk) { feed(); }
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!WdspChannel::workerHandoffHeldForTest() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const bool held = WdspChannel::workerHandoffHeldForTest();
    check(held && slicePackets == 1 && !pipeline->needsRepair(),
          "first real worker handoff is acknowledged before the next exchange");
    if (!held || slicePackets != 1 || pipeline->needsRepair()) { hold.release(); pipeline->stop(); return; }

    // An 8192-sample USB callback can produce seven WFM IQ blocks at the
    // slowest admitted capture rates. Prepared headroom must accept that finite
    // burst without needing the held worker to run. This fails on the old D2
    // exchange, independently of host speed or an arbitrary sleep duration.
    for (unsigned chunk = 0; chunk < 512 && slicePackets < 7 && !pipeline->needsRepair(); ++chunk) { feed(); }
    check(slicePackets == 7 && !pipeline->needsRepair(),
          "nonblocking WFM accepts the bounded seven-block USB burst while its worker is held");
    // D8 has seven prefilled output blocks plus the first acknowledged worker
    // handoff. Exhaustion therefore occurs on the ninth IQ block. It remains a
    // real fexchange2 -2; treating -2 as Ok must still fail this withdrawal.
    for (unsigned chunk = 0; chunk < 512 && !pipeline->needsRepair(); ++chunk) { feed(); }
    check(pipeline->needsRepair() && slicePackets == 8,
          "real nonblocking WFM exchange withdraws beyond its bounded eight-block credit");
    std::printf("WFM_HELD_BURST successful_blocks=%zu repair=%d\n", slicePackets, pipeline->needsRepair());
    const std::uint64_t faultCaptureFirst = failedCaptureFirst;
    // Subsequent continuous RF cannot publish stale decoder output or a second
    // first-fault record from the withdrawn receiver.
    for (unsigned chunk = 0; chunk < 8; ++chunk) { feed(); }
    check(slicePackets == 8, "withdrawn WFM receiver emits no stale independent PCM");
    unsigned faultEvents = 0;
    Pipeline::TraceEvent event;
    while (pipeline->takeTraceEvent(event)) {
        if (event.kind != Pipeline::TraceEvent::Kind::ReceiverFailure) { continue; }
        ++faultEvents;
        check(event.token == current.token && event.hardwareGeneration == current.capture.generation
                  && event.captureEpoch == firstSlice.captureEpoch && event.slot == 3 && event.stableId == 3
                  && event.instance == firstSlice.instance && event.receiverEpoch == firstSlice.receiverEpoch,
              "real underrun trace retains the accepted receiver and capture identity");
        check(event.captureFirst == faultCaptureFirst && event.captureFrames == 256,
              "real underrun trace names the exact failing capture callback");
        check(event.failure.has_value(), "real underrun trace carries a typed first failure");
        if (event.failure) {
            const auto& failure = *event.failure;
            check(failure.reason == Registry::ProcessingFailureReason::DspProcess
                      && failure.processResult == WdspChannel::ProcessResult::Underrun,
                  "first fault reports actual WDSP Underrun rather than a capture gap");
            check(failure.hasExpectedCaptureFirst && failure.expectedCaptureFirst == faultCaptureFirst
                      && failure.captureFirst == faultCaptureFirst && failure.captureFrames == 256
                      && failure.hasIqFirst && failure.iqFirst == 16384 && failure.iqFrames == 2048,
                  "first fault proves continuous capture and identifies the ninth exact IQ block");
            check(failure.extraction && failure.extraction->reason == Extractor::FailureReason::SinkRejected,
                  "RF extraction reports downstream rejection of the actual failing WDSP block");
        }
    }
    check(faultEvents == 1 && pipeline->diagnostics().droppedTraceEvents == 0,
          "one retained first-fault event survives later RF without diagnostic loss");
    hold.release(); // mandatory before control-side repair can retire the held worker
    current.token.revision++;
    const bool repaired = pipeline->prepare(current) && ready(*pipeline) && pipeline->adopt();
    check(repaired, "unchanged WFM recipe repairs after the real worker is released");
    if (repaired) {
        const auto audio = run(*pipeline, current, position, 2.0);
        check(audio[0].seen && audio[1].seen && audio[0].receiverEpoch != firstSlice.receiverEpoch
                  && audio[1].captureEpoch != firstSlice.captureEpoch
                  && audio[0].discontinuity && audio[1].discontinuity,
              "underrun repair replaces decoder and speaker histories");
        check(audio[0].stereo && amplitude(audio[0].left, 1000) > 0.001
                  && amplitude(audio[0].right, 2000) > 0.001
                  && amplitude(audio[1].left, 1000) > 0.001 && amplitude(audio[1].right, 2000) > 0.001,
              "released-worker repair delivers actual paired reference audio and pilot acquisition");
    }
    pipeline->stop();
}
void boundedBenchmark()
{
    // Opt-in pipeline plus production DDC/spectrum throughput qualification.
    // USB byte conversion, backend timers, AudioEngine and GUI are absent.
    // The 65536-bin display FFT at 25 RF frames/second, squelch detector,
    // extraction, WDSP worker, independent tap, mixer and packet drain run
    // sequentially on the same capture IQ. No synthetic firmware peer.
    constexpr std::uint64_t kCaptureRate = 2'400'000;
    constexpr std::uint64_t kRfSeconds = 4;
    constexpr std::size_t kChunk = 8192;
    constexpr double kFeedSpeed = 2.1;
    constexpr int kSpectrumFps = 25;
    constexpr std::uint64_t kNominalFrames = kRfSeconds * 48000;
    const Transaction::State current = state();
    std::vector<std::complex<float>> iq(kRfSeconds * kCaptureRate);
    // All fixture frequencies are integer kHz, so this analytically generated
    // one-millisecond period repeats exactly. Generation is outside the timer.
    std::array<std::complex<float>, kCaptureRate / 1000> period;
    for (std::size_t i = 0; i < period.size(); ++i) {
        period[i] = referenceIq(i, kCaptureRate);
    }
    for (std::size_t offset = 0; offset < iq.size(); offset += period.size()) {
        std::copy(period.begin(), period.end(), iq.begin() + offset);
    }

    // Determine the exact finite-run frame count, including resampler startup
    // and the incomplete final IQ block, without involving WDSP or timing.
    // This prevents an apparently fast run from qualifying by dropping audio.
    const Extractor::Config extractorConfig{current.capture,
        current.receivers.front().passband, 384000, 2048, 48000};
    Extractor oracle(extractorConfig);
    check(oracle.valid(), "benchmark frame-count extractor prepares");
    if (!oracle.valid()) { return; }
    ClockSink expected;
    for (std::size_t offset = 0; offset < iq.size(); offset += kChunk) {
        const std::size_t count = std::min(kChunk, iq.size() - offset);
        if (!oracle.process(current.capture, offset, std::span(iq).subspan(offset, count), expected)) {
            check(false, "benchmark frame-count extraction accepts the complete RF vector");
            return;
        }
    }
    const std::uint64_t expectedFrames = std::uint64_t(expected.blocks) * 256;
    check(expectedFrames <= kNominalFrames && expectedFrames > kNominalFrames - 1024,
          "finite benchmark output approaches four seconds at the declared 48 kHz rate");
    using Ddc = AetherSDR::rtl::RtlSdrDdc;
    Ddc ddc; // FFT plans, window preparation and signal wiring are untimed.
    ddc.applyCapture(kCaptureRate, current.capture.centerHz,
        current.receivers.front().passband.carrierHz, Transaction::Mode::Wfm, -100000, 100000,
        current.capture.usableLeftHz, current.capture.usableRightHz);
    ddc.setSpectrumRateFps(kSpectrumFps);
    std::uint64_t spectrumFrames = 0;
    std::uint64_t detectorFrames = 0;
    unsigned legacyAudioPackets = 0;
    QObject::connect(&ddc, &Ddc::spectrumFrameReady, &ddc,
        [&](int panId, const QByteArray& frame) {
            ++spectrumFrames;
            check(panId == 0 && frame.size() == Ddc::kSpectrumBinCount * qsizetype(sizeof(float)),
                  "benchmark computes complete production display FFT frames");
            bool finite = true;
            float peak = -std::numeric_limits<float>::infinity();
            for (qsizetype offset = 0; offset + qsizetype(sizeof(float)) <= frame.size(); offset += sizeof(float)) {
                float value;
                std::memcpy(&value, frame.constData() + offset, sizeof(value));
                finite &= std::isfinite(value);
                peak = std::max(peak, value);
            }
            check(finite && peak > -100,
                  "benchmark display FFT contains finite measured RF energy");
        });
    QObject::connect(&ddc, &Ddc::audioFrameReady, &ddc,
        [&](const QByteArray&, const QByteArray&) { ++legacyAudioPackets; });
    const std::uint64_t expectedSpectrumFrames = 1
        + (iq.size() - Ddc::kSpectrumBinCount) / (kCaptureRate / kSpectrumFps);
    QVector<std::complex<float>> captureBlock(kChunk);
    auto pipeline = std::make_unique<Pipeline>(1, true);
    const bool installed = pipeline->prepare(current, true) && ready(*pipeline) && pipeline->adopt();
    check(installed && !pipeline->legacy(), "benchmark prepares the real native WFM graph before timing");
    if (!installed || pipeline->legacy()) { pipeline->stop(); return; }
    std::array<Audio, 2> output;
    for (Audio& audio : output) {
        audio.left.reserve(kNominalFrames);
        audio.right.reserve(kNominalFrames);
    }
    std::uint64_t position = 0;
    const auto started = std::chrono::steady_clock::now();
    while (position < iq.size()) {
        const std::size_t count = std::min<std::uint64_t>(kChunk, iq.size() - position);
        captureBlock.resize(count); // Retains the preallocated chunk capacity.
        std::copy_n(iq.data() + position, count, captureBlock.data());
        // Match the worker's detector/display/audio ordering with DC suppression
        // off, and disable legacy DDC audio while native WFM owns reception.
        ddc.processSquelchSpectrum(captureBlock);
        ddc.processIqData(captureBlock, false, false);
        const auto spectrum = ddc.takeSquelchSpectrum();
        if (!spectrum.empty()) {
            ++detectorFrames;
            check(spectrum.size() == 2048, "benchmark retains the real squelch detector workload");
            pipeline->observeSpectrum(spectrum, position);
        }
        if (!pipeline->process(position, std::span(captureBlock.constData(), captureBlock.size()))
            || pipeline->needsRepair()) {
            check(false, "benchmark sustains real nonblocking WDSP processing without withdrawal");
            break;
        }
        position += count;
        Pipeline::Packet packet;
        while (pipeline->takePacket(packet)) {
            check(packet.token == current.token && (packet.slot == -1 || packet.slot == 3),
                  "benchmark output preserves current token and sparse receiver identity");
            output[packet.slot < 0 ? 1 : 0].accept(packet);
        }
        // An unlimited burst can outrun the nonblocking WDSP exchange worker.
        // Exercise a sustained >2x RF clock instead, and measure ALL feed and
        // drain work plus scheduling waits. The measured speed must still pass;
        // this cap is a throughput lower bound, not a maximum-speed claim.
        std::this_thread::sleep_until(started + std::chrono::duration<double>(
            double(position) / (kCaptureRate * kFeedSpeed)));
        if (std::chrono::steady_clock::now() - started > 5s) {
            check(false, "benchmark completes within its bounded feed interval");
            break;
        }
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const double rfSeconds = double(position) / kCaptureRate;
    const double speed = rfSeconds / elapsed;
    const auto diagnostics = pipeline->diagnostics();
    std::printf("WFM_BENCHMARK scope=pipeline_ddc_fft25 capture_rate=%llu rf_seconds=%.6f elapsed_seconds=%.6f "
        "speed=%.3fx feed_cap=%.1fx expected_frames=%llu slice_frames=%zu speaker_frames=%zu "
        "repair=%d drops=%llu late_frames=%llu mixer_rejections=%llu mixer_config_failures=%llu "
        "fft_fps=%d expected_fft_frames=%llu fft_frames=%llu detector_frames=%llu "
        "ddc_legacy_audio_packets=%u\n",
        static_cast<unsigned long long>(kCaptureRate), rfSeconds, elapsed, speed, kFeedSpeed,
        static_cast<unsigned long long>(expectedFrames), output[0].left.size(), output[1].left.size(),
        pipeline->needsRepair(), static_cast<unsigned long long>(diagnostics.droppedPackets),
        static_cast<unsigned long long>(diagnostics.mixerLateFrames),
        static_cast<unsigned long long>(diagnostics.mixerRejectedBlocks),
        static_cast<unsigned long long>(diagnostics.mixerConfigurationFailures), kSpectrumFps,
        static_cast<unsigned long long>(expectedSpectrumFrames), static_cast<unsigned long long>(spectrumFrames),
        static_cast<unsigned long long>(detectorFrames),
        legacyAudioPackets);
    check(spectrumFrames == expectedSpectrumFrames
              && detectorFrames > 0 && legacyAudioPackets == 0,
          "benchmark includes every scheduled 25 FPS display FFT and excludes duplicate legacy audio");
    check(position == iq.size() && rfSeconds >= 4.0 && speed >= 2.0,
          "benchmark completes at least four RF seconds at measured >=2x real time");
    check(diagnostics.observed && !pipeline->needsRepair() && diagnostics.droppedPackets == 0
              && diagnostics.mixerLateFrames == 0 && diagnostics.mixerRejectedBlocks == 0
              && diagnostics.mixerConfigurationFailures == 0,
          "benchmark has no receiver faults, packet-mailbox loss or mixer failure");
    for (const Audio& audio : output) {
        check(audio.left.size() == expectedFrames && audio.right.size() == expectedFrames
                  && audio.next == expectedFrames,
              "benchmark delivers every expected contiguous 48 kHz frame to slice and speaker");
        check(amplitude(audio.left, 1000) > 0.001 && amplitude(audio.right, 2000) > 0.001
                  && audio.peak < 1 && audio.clipped == 0,
              "benchmark output contains finite unclipped reference audio rather than empty packets");
    }
    check(output[0].stereo, "benchmark preserves actual WFM pilot acquisition under the faster RF clock");
    pipeline->stop();
}
void admission()
{
    auto legacy = std::make_unique<Pipeline>();
    const Transaction::State current = state();
    check(legacy->prepare(current, true) && ready(*legacy) && legacy->adopt()
              && legacy->legacy() == !Pipeline::kQualifiedWfmEnabled,
          "default pipeline follows the explicit qualification gate");
    legacy->stop();
    auto native = std::make_unique<Pipeline>(1, true);
    Transaction::State invalid = current;
    invalid.receivers[0].wfmDeemphasisUs = 51;
    check(!native->prepare(invalid), "unsupported deemphasis is refused before preparation");
    invalid = current;
    invalid.receivers[0].squelchEnabled = true;
    check(!native->prepare(invalid), "manual narrow-FM squelch cannot masquerade as WFM internal squelch");
    invalid = current;
    invalid.receivers.push_back(invalid.receivers[0]);
    invalid.receivers.back().passband.stableId = 4;
    invalid.receivingIds.push_back(4);
    check(!native->prepare(invalid), "WFM qualification does not increase production receiver admission");
    native->stop();
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    outputClockAlignment();
    admission();
    nativeRouting();
    workerUnderrunTrace();
    if (qEnvironmentVariableIntValue("AETHER_WFM_BENCHMARK") == 1) { boundedBenchmark(); }
    QThreadPool::globalInstance()->waitForDone();
    std::printf("rtl_wfm_pipeline_test: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
