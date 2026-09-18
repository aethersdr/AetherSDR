// Socket-free producer contract: owned samples, format/identity/continuity,
// delayed delivery and fail-closed compatibility. No radio or audio device.
#include "core/PcmFrame.h"

#include <QCoreApplication>
#include <QEvent>
#include <cstdio>
#include <barrier>
#include <thread>

using namespace AetherSDR;

namespace {
int failures = 0;
int checks = 0;
void check(bool pass, const char* name)
{
    ++checks;
    if (!pass) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", name);
    }
}
QByteArray bytes(const QVector<float>& samples)
{
    return {reinterpret_cast<const char*>(samples.constData()),
            samples.size() * static_cast<qsizetype>(sizeof(float))};
}

void formatsAndOwnership()
{
    PcmProducer producer;
    check(!producer.legacyStereo24(bytes({0.1f, -0.1f})), "unstarted producer refuses PCM");
    check(producer.start(), "start legacy producer");
    const QVector<float> samples{0.0f, -0.0f, 1.25f, -1.5f, 0.123f, -0.456f};
    QByteArray input = bytes(samples);
    const auto first = producer.legacyStereo24(input);
    check(first.has_value(), "valid legacy stereo accepted");
    check(first->stream().format == PcmFormat{}, "producer rate stays 24 kHz stereo");
    check(first->frameCount() == 3 && first->firstSample() == 0 && first->discontinuity(),
          "frame count counts LR pairs and first frame marks a discontinuity");
    check(first->legacyStereo24() == input, "byte exact, including signed zero and unclipped peaks");
    const QByteArray borrowed = QByteArray::fromRawData(input.constData(), input.size());
    const auto owned = producer.legacyStereo24(borrowed);
    input.fill(0);
    check(owned->legacyStereo24() == bytes(samples), "queued frame owns borrowed callback samples");
    check(owned->firstSample() == 3 && !owned->discontinuity(), "legacy sample position advances");
    check(!producer.legacyStereo24({}), "empty bytes rejected");
    for (int n = 1; n < 8; ++n) {
        check(!producer.legacyStereo24(QByteArray(n, '\0')), "partial stereo frame rejected");
    }
    check(!producer.legacyStereo24(QByteArray((PcmFrame::kMaxFrames + 1) * 8, '\0')),
          "oversized bytes rejected before copying");
    check(!producer.produce({1.0f}), "odd stereo sample count rejected");
    check(!producer.produce({std::numeric_limits<float>::quiet_NaN(), 0.0f}), "NaN rejected");
    check(!producer.produce({0.0f, std::numeric_limits<float>::infinity()}), "infinity rejected");
    const auto next = producer.produce({0.25f, -0.25f});
    check(next->firstSample() == 6, "malformed input never advances the cursor");
    check(!producer.setFormat({44100, PcmLayout::Stereo}), "unsupported producer rate refused");
    check(!producer.setFormat({24000, static_cast<PcmLayout>(9)}), "unknown layout refused");
    check(next->current(), "refused format change preserves valid stream");
    check(producer.setFormat({48000, PcmLayout::Stereo}), "48 kHz contract supported");
    check(!next->current() && next->legacyStereo24().isEmpty(), "old-format queued PCM revoked");
    const auto wide = producer.produce({0.75f, -0.75f});
    check(wide->stream().formatGeneration == 2 && wide->firstSample() == 0,
          "new format has new generation and sample origin");
    check(wide->legacyStereo24().isEmpty(), "legacy consumer refuses 48 kHz, never relabels");
    check(!producer.legacyStereo24(bytes({0.5f, -0.5f})), "24 kHz adapter cannot label bytes as 48");
    check(producer.setFormat({24000, PcmLayout::Stereo}), "return to 24 kHz");
    const auto returned = producer.produce({0.5f, -0.5f});
    check(returned->stream().formatGeneration == 3 && !wide->current(), "format ABA rejected");
    check(producer.setFormat({48000, PcmLayout::Mono}), "mono native PCM supported");
    const auto mono = producer.produce({0.0f, 0.25f, -0.5f});
    check(mono->frameCount() == 3 && mono->legacyStereo24().isEmpty(), "mono count and legacy refusal");
#if QT_VERSION >= QT_VERSION_CHECK(6, 11, 0)
    float borrowedSamples[]{0.5f, -0.5f};
    const auto borrowedFrame = producer.produce(QVector<float>::fromReadOnlyData(borrowedSamples));
    borrowedSamples[0] = 0.0f;
    check(borrowedFrame->samples()[0] == 0.5f, "native frame owns external Qt container storage");
#endif
    check(!producer.start(PcmPurpose::Slice, -1), "slice requires stable slot");
    check(!producer.start(PcmPurpose::Speaker, 0), "speaker cannot masquerade as a slice");
    check(!producer.start(static_cast<PcmPurpose>(10)), "unknown purpose rejected");
}

void identityContinuityAndQueueing(QObject& receiver)
{
    PcmProducer producer;
    producer.start(PcmPurpose::Slice, 3, {}, 700, 900);
    const auto first = producer.produce({0.1f, -0.1f});
    check(first->stream().session == 700 && first->stream().receiverInstance == 900
          && first->stream().sliceId == 3, "F4 session, receiver instance and stable slot remain separate");
    check(!producer.start(PcmPurpose::Slice, 3, {}, 700, 901), "repeated explicit session refused");
    check(!producer.start(PcmPurpose::Slice, 3, {}, 699, 902), "older explicit session refused");
    check(first->current(), "refused stale start preserves current epoch");
    PcmFrameGate gate;
    check(gate.accept(*first), "first delivery accepted");
    check(!gate.accept(*first), "duplicate delivery refused");
    check(!producer.produce({0.1f, -0.1f}, 0, true), "discontinuity cannot authorize backwards PCM");
    check(!producer.produce({0.1f, -0.1f}, 4), "unmarked sample gap refused");
    const auto gap = producer.produce({0.1f, -0.1f}, 4, true);
    check(gap && gate.accept(*gap), "marked forward discontinuity accepted");
    check(!producer.produce({0.1f, -0.1f}, std::numeric_limits<quint64>::max(), true),
          "sample position overflow refused");

    // A consumer detached from a running producer resumes when it is
    // reattached. The producer counts on production, not delivery, so the
    // frames it emitted while this gate was starved leave a forward gap that
    // carries no discontinuity flag — the exact shape a playback mute/unmute
    // cycle produces on both the Flex and seam-backend speaker paths. The gate
    // must resync rather than refuse: the cursor only moves on an accepted
    // frame, so one refusal here would silence the consumer for the life of
    // the epoch. Backward motion stays refused (asserted below).
    {
        PcmProducer live;
        live.start(PcmPurpose::Speaker);
        PcmFrameGate attached;
        for (int i = 0; i < 3; ++i) {
            const auto frame = live.produce({0.3f, -0.3f});
            check(frame && attached.accept(*frame), "attached consumer accepts steady frames");
        }
        // Detached: produced, never delivered to this gate.
        for (int i = 0; i < 5; ++i) {
            check(live.produce({0.3f, -0.3f}).has_value(), "producer advances while consumer is detached");
        }
        const auto resumed = live.produce({0.3f, -0.3f});
        check(resumed && !resumed->discontinuity(),
              "resumed frame carries no discontinuity flag");
        check(resumed && resumed->firstSample() > 3,
              "resumed frame is ahead of the starved cursor");
        check(resumed && attached.accept(*resumed),
              "consumer reattached after a forward gap resyncs instead of locking out");
        int accepted = 0;
        for (int i = 0; i < 5; ++i) {
            const auto frame = live.produce({0.3f, -0.3f});
            if (frame && attached.accept(*frame)) {
                ++accepted;
            }
        }
        check(accepted == 5, "reattached consumer keeps accepting after resync");
    }
    int deliveries = 0;
    QMetaObject::invokeMethod(&receiver, [frame = *gap, &deliveries] {
        if (!frame.legacyStereo24().isEmpty()) {
            ++deliveries;
        }
    }, Qt::QueuedConnection);
    producer.start(PcmPurpose::Slice, 3, {}, 701, 901);
    QCoreApplication::sendPostedEvents(&receiver, QEvent::MetaCall);
    check(deliveries == 0, "queued old occupant rejected after same-slot reconnect");
    check(!gate.accept(*gap), "stale frame cannot re-admit old session");
    const auto replacement = producer.produce({0.2f, -0.2f});
    check(gate.accept(*replacement), "new same-slot occupant accepted");
    check(replacement->stream().source == first->stream().source
          && replacement->stream().session != first->stream().session,
          "producer identity survives session change");
    producer.invalidate();
    check(!gate.accept(*replacement), "removed source rejected");
    check(!producer.produce({0.1f, -0.1f}), "stopped producer cannot publish");
    PcmFrame retained;
    {
        PcmProducer scoped;
        scoped.start();
        retained = *scoped.produce({0.1f, -0.1f});
    }
    check(!retained.current(), "destruction revokes pending PCM without retaining producer");
}

void concurrentRevocation()
{
    PcmProducer producer;
    producer.start();
    const PcmFrame retained = *producer.produce({0.25f, -0.5f});
    std::barrier rendezvous(2);
    bool samplesIntact = true;
    bool refusedAfterRevocation = false;
    std::thread consumer([&] {
        rendezvous.arrive_and_wait();
        // Read the epoch while its owner revokes it. There is deliberately no
        // ordering between these reads and invalidate(); TSan must see the
        // token's atomic synchronization, not a test-only mutex around it.
        for (int i = 0; i < 10000; ++i) {
            const bool current = retained.current();
            const QByteArray pcm = retained.legacyStereo24();
            samplesIntact = samplesIntact && retained.samples()[0] == 0.25f
                && retained.samples()[1] == -0.5f
                && (pcm.isEmpty() || pcm == bytes({0.25f, -0.5f}));
            (void)current;
        }
        rendezvous.arrive_and_wait();
        refusedAfterRevocation = !producer.produce({0.25f, -0.5f});
    });
    rendezvous.arrive_and_wait();
    producer.invalidate();
    rendezvous.arrive_and_wait();
    consumer.join();
    check(samplesIntact, "concurrent revocation never changes owned sample bytes");
    check(refusedAfterRevocation && !retained.current(), "revocation reaches another execution context");
}

void boundedIndependentStreams()
{
    PcmFrameGate gate;
    std::array<std::unique_ptr<PcmProducer>, PcmFrameGate::kMaxStreams + 1> producers;
    for (std::size_t i = 0; i < producers.size(); ++i) {
        producers[i] = std::make_unique<PcmProducer>();
        producers[i]->start();
        const auto frame = producers[i]->produce({0.1f, -0.1f});
        check(gate.accept(*frame) == (i < PcmFrameGate::kMaxStreams), "bounded source admission");
    }
    producers[0]->invalidate();
    check(gate.accept(*producers.back()->produce({0.2f, -0.2f})), "retired cursor reused at capacity");
    const auto frame = producers[1]->produce({0.2f, -0.2f});
    PcmFrameGate independentTap;
    check(gate.accept(*frame) && independentTap.accept(*frame), "consumers have independent cursors");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<PcmFrame>();
    check(QMetaType::fromName("AetherSDR::PcmFrame").isValid(), "name-based metatype registered");
    formatsAndOwnership();
    identityContinuityAndQueueing(app);
    boundedIndependentStreams();
    concurrentRevocation();
    std::printf("PCM contract: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
