// Socket-free fixed24 decoder ingress: real PcmProducer and Resampler, no
// firmware peers, devices, clocks or settings.
#include "core/DecoderPcmAdapter.h"

#include <QCoreApplication>
#include <QEvent>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numbers>
#include <vector>

using namespace AetherSDR;

namespace {
int failures = 0;
int checks = 0;
using Lane = DecoderPcmAdapter::RouteLane;

void check(bool passed, const char* name)
{
    ++checks;
    if (!passed) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", name);
    }
}

void passthroughAndAdmission(QObject& receiver)
{
    PcmProducer mono;
    mono.start(PcmPurpose::Slice, 4, {24000, PcmLayout::Mono}, 8, 11);
    const auto first = mono.produce({0.0f, -0.0f, 1.25f, -2.0f});
    DecoderPcmAdapter adapter;
    check(!adapter.accept(PcmFrame{}), "invalid frame refused");
    check(!adapter.accept(*first), "route selection is explicit");
    check(!adapter.selectRoute(Lane::NativeSlice, -1), "negative selection refused");
    check(adapter.selectRoute(Lane::NativeSlice, 4), "native selection accepted");
    const auto block = adapter.accept(*first);
    check(block.has_value(), "selected mono24 accepted");
    if (!block) {
        return;
    }
    check(block->samples.size() == 4
          && std::memcmp(block->samples.constData(), first->samples().constData(), 16) == 0,
          "mono24 preserves exact finite sample bits including negative zero and peaks");
    check(block->segmentFirstInputSample == 0 && block->inputEndSample == 4
          && block->firstOutputSample == 0 && block->inputSampleRateHz == 24000
          && block->groupDelayInputFrames == 0 && block->discontinuity,
          "passthrough positions count source frames with zero delay");
    check(block->source.stream() == first->stream() && block->current(),
          "output retains original live descriptor");
    check(!adapter.accept(*first), "duplicate rejected");
    adapter.reset();
    check(!adapter.accept(*first), "reset preserves replay refusal");
    const auto next = mono.produce({0.25f});
    const auto afterReset = adapter.accept(*next);
    check(afterReset && afterReset->discontinuity
          && afterReset->segmentFirstInputSample == 4 && afterReset->firstOutputSample == 0,
          "reset begins new segment at next accepted input");

    bool revokedAtDelivery = false;
    QMetaObject::invokeMethod(&receiver, [block, &revokedAtDelivery] {
        revokedAtDelivery = !block->current();
    }, Qt::QueuedConnection);
    mono.invalidate();
    QCoreApplication::sendPostedEvents(&receiver, QEvent::MetaCall);
    check(revokedAtDelivery, "converted queued output observes source revocation");
    check(!adapter.accept(*next), "revoked input refused");

    PcmProducer stereo;
    stereo.start(PcmPurpose::Slice, 4);
    const float peak = std::numeric_limits<float>::max();
    const auto stereoFrame = stereo.produce({0.75f, -0.25f, peak, peak, -peak, peak});
    const auto average = adapter.accept(*stereoFrame);
    check(average && average->samples == QVector<float>({0.25f, peak, 0.0f})
          && average->inputEndSample == 3,
          "stereo arithmetic mean uses halves to avoid finite input overflow");

    PcmProducer other;
    other.start(PcmPurpose::Slice, 4);
    const auto competing = other.produce({0.1f, 0.1f});
    check(!adapter.accept(*competing), "another live source cannot replace selected route pin");
    adapter.clearRoute();
    adapter.selectRoute(Lane::NativeSlice, 4);
    check(!adapter.accept(*competing), "selection toggles preserve live source pin");
    adapter.retireRoute(Lane::NativeSlice, 4);
    const auto retired = stereo.produce({0.25f, 0.25f});
    check(!adapter.accept(*retired), "retirement clears selection");
    adapter.selectRoute(Lane::NativeSlice, 4);
    check(!adapter.accept(*retired), "reselection cannot resurrect retired live source");
    check(adapter.accept(*competing).has_value(), "explicitly reselected route admits new receiver source");

    PcmProducer wrongSlot;
    wrongSlot.start(PcmPurpose::Slice, 5);
    check(!adapter.accept(*wrongSlot.produce({0.1f, 0.1f})), "native metadata must match selected slice");
    PcmProducer dax;
    dax.start(PcmPurpose::Auxiliary);
    const auto daxFrame = dax.produce({0.1f, 0.1f});
    check(!adapter.accept(*daxFrame), "auxiliary DAX cannot enter native lane");
    adapter.selectRoute(Lane::Dax, 4);
    check(adapter.accept(*daxFrame).has_value(), "same-number DAX selection has a distinct lane");
    check(!adapter.accept(*competing), "native PCM cannot enter DAX lane");
    adapter.selectRoute(Lane::NativeSlice, 4);
    check(!adapter.accept(*competing), "route changes preserve earlier replay cursor");
}

QVector<float> tone(int rate, double hz, int count)
{
    QVector<float> samples(count);
    for (int index = 0; index < count; ++index) {
        samples[index] = static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * index / rate));
    }
    return samples;
}

QVector<float> convert(const QVector<float>& mono, int rate, PcmLayout layout,
                       const std::vector<int>& chunks, int* delay = nullptr)
{
    PcmProducer producer;
    producer.start(PcmPurpose::Slice, 0, {rate, layout});
    DecoderPcmAdapter adapter;
    adapter.selectRoute(Lane::NativeSlice, 0);
    QVector<float> output;
    int offset = 0;
    std::size_t chunkIndex = 0;
    while (offset < mono.size()) {
        const int count = std::min(chunks[chunkIndex++ % chunks.size()],
                                   static_cast<int>(mono.size()) - offset);
        QVector<float> input;
        input.reserve(count * (layout == PcmLayout::Mono ? 1 : 2));
        for (int index = 0; index < count; ++index) {
            // One-channel stereo signal exercises a real average, not merely
            // duplicate channels that also pass a "use left" mutation.
            input.append(layout == PcmLayout::Mono ? mono[offset + index] : 2 * mono[offset + index]);
            if (layout == PcmLayout::Stereo) {
                input.append(0.0f);
            }
        }
        const auto block = adapter.accept(*producer.produce(std::move(input)));
        check(block && block->inputSampleRateHz == rate
              && block->inputEndSample == static_cast<quint64>(offset + count)
              && block->segmentFirstInputSample == 0
              && block->firstOutputSample == static_cast<quint64>(output.size())
              && block->discontinuity == (offset == 0),
              "arbitrary chunks retain exact producer end and decoder sample positions");
        if (!block) {
            return {};
        }
        if (delay) {
            *delay = block->groupDelayInputFrames;
        }
        output += block->samples;
        offset += count;
    }
    return output;
}

double rms(const QVector<float>& samples, int first)
{
    double sum = 0;
    for (qsizetype index = first; index < samples.size(); ++index) {
        sum += static_cast<double>(samples[index]) * samples[index];
    }
    return std::sqrt(sum / (samples.size() - first));
}

void continuousConversion()
{
    PcmProducer tiny;
    tiny.start(PcmPurpose::Slice, 1, {48000, PcmLayout::Mono});
    DecoderPcmAdapter adapter;
    adapter.selectRoute(Lane::NativeSlice, 1);
    const auto first = adapter.accept(*tiny.produce({1.0f}, 1000, true));
    check(first && first->samples.isEmpty() && first->discontinuity
          && first->segmentFirstInputSample == 1000 && first->inputEndSample == 1001
          && first->firstOutputSample == 0 && first->groupDelayInputFrames > 0,
          "single 48k frame accepts empty output with immediate reset and timing anchor");
    const auto almost = adapter.accept(*tiny.produce(QVector<float>(254, 0.0f)));
    check(almost && almost->samples.isEmpty() && !almost->discontinuity
          && almost->inputEndSample == 1255,
          "partial 48k batch stays staged without padding");
    const auto complete = adapter.accept(*tiny.produce({0.0f}));
    check(complete && complete->samples.size() == 128 && complete->firstOutputSample == 0
          && complete->inputEndSample == 1256,
          "256 accumulated source frames emit 128 decoder frames");

    constexpr int count = 32768;
    const QVector<float> input = tone(48000, 1100, count);
    int delay = 0;
    const QVector<float> steady = convert(input, 48000, PcmLayout::Mono, {256}, &delay);
    const QVector<float> varied = convert(input, 48000, PcmLayout::Mono, {1, 3, 255, 257, 4095, 9});
    const QVector<float> stereo = convert(input, 48000, PcmLayout::Stereo, {1, 511, 19, 4096});
    check(steady.size() == count / 2, "continuous48 output duration is exactly half the input frame count");
    check(varied == steady && stereo == steady, "odd chunks and stereo layouts preserve continuous filter history");
    if (steady.size() > 2048) {
        check(std::abs(rms(steady, 2048) - std::sqrt(0.5)) < 0.002,
              "48k passband tone keeps gain in fixed24 domain");
        double error = 0;
        for (int index = 2048; index < steady.size(); ++index) {
            const double expected = std::sin(2.0 * std::numbers::pi * 1100
                * (2.0 * index - delay) / 48000.0);
            error += std::pow(steady[index] - expected, 2);
        }
        check(std::sqrt(error / (steady.size() - 2048)) < 0.002,
              "reported source-frame group delay maps output tone phase precisely");
    }
    QVector<float> impulse(count, 0.0f);
    impulse[4096] = 1.0f;
    const QVector<float> response = convert(impulse, 48000, PcmLayout::Mono, {17, 31, 1023}, &delay);
    if (!response.isEmpty()) {
        const auto peak = std::max_element(response.cbegin(), response.cend(),
            [](float left, float right) { return std::abs(left) < std::abs(right); });
        const double peakIndex = std::distance(response.cbegin(), peak);
        check(std::abs(2 * peakIndex - 4096 - delay) <= 1,
              "continuous impulse locates documented acoustic group delay");
    }
    const QVector<float> rejected = convert(tone(48000, 16000, count), 48000,
                                           PcmLayout::Mono, {257, 4095});
    check(rejected.size() > 2048 && rms(rejected, 2048) < 0.0001,
          "48-to24 filter rejects out-of-band tone before it aliases");
    const QVector<float> native = tone(24000, 1100, 8192);
    check(convert(native, 24000, PcmLayout::Mono, {1, 257, 77}) == native
          && convert(native, 24000, PcmLayout::Stereo, {511, 9}) == native,
          "24k mono and one-sided stereo preserve tone samples and duration");
}

bool zeroOutput(const std::optional<DecoderPcmBlock>& block)
{
    return block && !block->samples.isEmpty()
        && std::all_of(block->samples.cbegin(), block->samples.cend(),
                       [](float sample) { return sample == 0.0f; });
}

void transitionHistoryAndBounds()
{
    PcmProducer producer;
    producer.start(PcmPurpose::Slice, 0, {48000, PcmLayout::Mono});
    DecoderPcmAdapter adapter;
    adapter.selectRoute(Lane::NativeSlice, 0);
    adapter.accept(*producer.produce(QVector<float>(255, 1.0f)));
    adapter.reset();
    const auto afterReset = adapter.accept(*producer.produce(QVector<float>(4096, 0.0f)));
    check(zeroOutput(afterReset) && afterReset->discontinuity
          && afterReset->segmentFirstInputSample == 255 && afterReset->firstOutputSample == 0,
          "reset discards partial staging and acoustic history without flushing a tail");
    adapter.accept(*producer.produce(QVector<float>(4096, 1.0f)));
    producer.produce(QVector<float>(33, 1.0f)); // consumer misses an unmarked live block
    const auto afterGap = adapter.accept(*producer.produce(QVector<float>(4096, 0.0f)));
    check(zeroOutput(afterGap) && afterGap->discontinuity && afterGap->firstOutputSample == 0
          && afterGap->segmentFirstInputSample == 8480,
          "unmarked forward gap resets both filter history and output position");
    adapter.accept(*producer.produce(QVector<float>(512, 1.0f)));
    const auto marked = adapter.accept(*producer.produce(QVector<float>(4096, 0.0f),
                                                         std::nullopt, true));
    check(zeroOutput(marked) && marked->discontinuity,
          "explicit discontinuity at contiguous position resets filter history");
    producer.setFormat({24000, PcmLayout::Mono});
    const auto narrowFrame = producer.produce({0.25f});
    const auto narrow = adapter.accept(*narrowFrame);
    check(narrow && narrow->samples == QVector<float>({0.25f}) && narrow->discontinuity
          && narrow->firstOutputSample == 0 && narrow->groupDelayInputFrames == 0,
          "48-to24 epoch transition removes converter history and delay");
    producer.setFormat({48000, PcmLayout::Stereo});
    const auto wide = adapter.accept(*producer.produce(QVector<float>(8192, 0.0f)));
    check(zeroOutput(wide) && wide->discontinuity && wide->source.stream().formatGeneration == 3
          && wide->segmentFirstInputSample == 0 && wide->firstOutputSample == 0,
          "rate and layout ABA creates a fresh converter epoch");
    check(!adapter.accept(*narrowFrame), "revoked format cannot enter a new epoch");

    PcmProducer other;
    other.start(PcmPurpose::Slice, 1, {48000, PcmLayout::Mono});
    adapter.accept(*producer.produce(QVector<float>(510, 1.0f)));
    adapter.selectRoute(Lane::NativeSlice, 1);
    const auto switched = adapter.accept(*other.produce(QVector<float>(4096, 0.0f)));
    check(zeroOutput(switched) && switched->discontinuity,
          "slice selection cannot mix the previous slice tail or partial staging");
    adapter.selectRoute(Lane::NativeSlice, 0);
    const auto returned = adapter.accept(*producer.produce(QVector<float>(8192, 0.0f)));
    check(zeroOutput(returned) && returned->discontinuity,
          "switching back also starts an independent history");
    producer.start(PcmPurpose::Slice, 0, {48000, PcmLayout::Stereo});
    const auto reconnected = adapter.accept(*producer.produce(QVector<float>(8192, 0.0f)));
    check(zeroOutput(reconnected) && reconnected->discontinuity
          && reconnected->source.stream().session == 2,
          "reconnect reuses slot with a fresh session");

    DecoderPcmAdapter bounded;
    std::vector<std::unique_ptr<PcmProducer>> live;
    for (int slot = 0; slot < 32; ++slot) {
        auto source = std::make_unique<PcmProducer>();
        source->start(PcmPurpose::Slice, slot, {24000, PcmLayout::Mono});
        bounded.selectRoute(Lane::NativeSlice, slot);
        check(bounded.accept(*source->produce({0.25f})).has_value(),
              "bounded pin table admits each of 32 live routes");
        live.push_back(std::move(source));
    }
    PcmProducer extra;
    extra.start(PcmPurpose::Slice, 32, {24000, PcmLayout::Mono});
    const auto extraFrame = extra.produce({0.25f});
    bounded.selectRoute(Lane::NativeSlice, 32);
    check(!bounded.accept(*extraFrame), "33rd live route is refused without evicting active pins");
    bounded.reset();
    check(!bounded.accept(*extraFrame), "reset cannot bypass live route bound");
    live[0]->invalidate();
    check(bounded.accept(*extraFrame).has_value(), "inactive route slot is reclaimed without consuming refused input");
    for (int cycle = 0; cycle < 64; ++cycle) {
        extra.start(PcmPurpose::Slice, 32, {24000, PcmLayout::Mono});
        check(bounded.accept(*extra.produce({0.5f})).has_value(),
              "revoked epochs do not exhaust persistent replay or source pins");
    }
}

void independentConsumers()
{
    PcmProducer source;
    source.start(PcmPurpose::Slice, 0, {48000, PcmLayout::Mono});
    DecoderPcmAdapter cw;
    DecoderPcmAdapter rtty;
    DecoderPcmAdapter clock;
    cw.selectRoute(Lane::NativeSlice, 0);
    rtty.selectRoute(Lane::NativeSlice, 0);
    clock.selectRoute(Lane::NativeSlice, 0);
    const auto input = source.produce(tone(48000, 750, 8192));
    const auto a = cw.accept(*input);
    const auto b = rtty.accept(*input);
    const auto c = clock.accept(*input);
    check(a && b && c && a->samples == b->samples && b->samples == c->samples,
          "CW RTTY and Clock may consume the same source independently");
    cw.reset();
    const auto next = source.produce(QVector<float>(4096, 0.0f));
    const auto resetCw = cw.accept(*next);
    const auto continuingRtty = rtty.accept(*next);
    const auto continuingClock = clock.accept(*next);
    check(zeroOutput(resetCw) && continuingRtty && continuingClock
          && continuingRtty->samples == continuingClock->samples
          && !zeroOutput(continuingClock) && resetCw->discontinuity
          && !continuingClock->discontinuity,
          "resetting one decoder does not reset another consumer's acoustic or timeline history");
}

void extremeInputAndOrdering()
{
    PcmProducer producer;
    producer.start(PcmPurpose::Slice, 2, {48000, PcmLayout::Mono});
    DecoderPcmAdapter adapter;
    adapter.selectRoute(Lane::NativeSlice, 2);
    adapter.accept(*producer.produce(QVector<float>(4096, 0.0f)));
    QVector<float> edge(8192, std::numeric_limits<float>::max());
    std::fill(edge.begin(), edge.begin() + 4096, 0.0f);
    const auto overflow = adapter.accept(*producer.produce(std::move(edge)));
    check(overflow && overflow->samples.isEmpty() && overflow->discontinuity
          && overflow->firstOutputSample == 0 && overflow->segmentFirstInputSample == 4096
          && overflow->inputEndSample == 12288 && overflow->current(),
          "filter overflow publishes immediate empty reset event with original live input anchor");
    const auto clean = adapter.accept(*producer.produce(QVector<float>(4096, 0.0f)));
    check(zeroOutput(clean) && clean->discontinuity && clean->firstOutputSample == 0
          && clean->segmentFirstInputSample == 12288,
          "numerical refusal retires contaminated converter history before subsequent delivery");
    producer.setFormat({24000, PcmLayout::Mono});
    const auto early = producer.produce({0.25f});
    const auto late = producer.produce({0.5f});
    check(adapter.accept(*late).has_value() && !adapter.accept(*early),
          "later admission permanently rejects older queued input");
    const auto nearEnd = producer.produce({0.75f}, std::numeric_limits<quint64>::max() - 1, true);
    const auto end = adapter.accept(*nearEnd);
    check(end && end->segmentFirstInputSample == std::numeric_limits<quint64>::max() - 1
          && end->inputEndSample == std::numeric_limits<quint64>::max()
          && end->firstOutputSample == 0,
          "large exact source positions preserve integer end arithmetic");
}
} // namespace

// The shared receive stream carries every audible slice already mixed, so its
// lane keys on purpose alone and must refuse both isolated lanes' frames.
void sharedRxDemodLane()
{
    PcmProducer speaker;
    PcmProducer slice;
    PcmProducer dax;
    speaker.start(PcmPurpose::Speaker, -1, {24000, PcmLayout::Stereo});
    slice.start(PcmPurpose::Slice, 3, {24000, PcmLayout::Mono});
    dax.start(PcmPurpose::Auxiliary, -1, {24000, PcmLayout::Mono});

    DecoderPcmAdapter adapter;
    check(adapter.selectRoute(Lane::RxDemod, 0), "shared receive lane selectable");
    check(!adapter.accept(*slice.produce({0.5f})),
          "shared lane refuses per-slice frames");
    check(!adapter.accept(*dax.produce({0.5f})),
          "shared lane refuses DAX auxiliary frames");
    const auto mixed = adapter.accept(*speaker.produce({0.5f, 0.25f}));
    check(mixed.has_value() && mixed->samples.size() == 1
          && std::fabs(mixed->samples[0] - 0.375f) < 1e-6f,
          "shared lane downmixes the stereo receive stream to mono24");

    // Selecting an isolated lane must not keep admitting the shared stream.
    check(adapter.selectRoute(Lane::Dax, 1), "isolated DAX selection accepted");
    check(!adapter.accept(*speaker.produce({0.5f, 0.25f})),
          "an isolated lane refuses the shared receive stream");
    check(adapter.accept(*dax.produce({0.5f})).has_value(),
          "isolated DAX lane still admits its own producer");
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QObject receiver;
    passthroughAndAdmission(receiver);
    sharedRxDemodLane();
    continuousConversion();
    transitionHistoryAndBounds();
    independentConsumers();
    extremeInputAndOrdering();
    std::printf("decoder_pcm_adapter_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
