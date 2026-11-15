#include "core/ReceivePresentationSync.h"

#include <QByteArray>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

int fail(const char* message)
{
    std::fprintf(stderr, "receive_presentation_sync_test: %s\n", message);
    return 1;
}

QVector<float> syntheticSignal(int samples)
{
    QVector<float> signal;
    signal.reserve(samples);
    quint32 state = 0x12345678u;
    for (int i = 0; i < samples; ++i) {
        state = state * 1664525u + 1013904223u;
        const float noise =
            static_cast<float>((state >> 8) & 0xffffu) / 32768.0f - 1.0f;
        const float tone =
            0.35f * std::sin(static_cast<float>(i) * 0.071f)
            + 0.23f * std::sin(static_cast<float>(i) * 0.173f);
        signal.append(tone + 0.08f * noise);
    }
    return signal;
}

float syntheticEnvelopeAt(int i)
{
    return 0.55f
           + 0.24f * std::sin(static_cast<float>(i) * 0.009f)
           + 0.16f * std::max(0.0f,
                              std::sin(static_cast<float>(i) * 0.041f));
}

} // namespace

int main()
{
    using namespace AetherSDR;

    ReceivePresentationSync sync;
    if (sync.delayMs(ReceivePresentationSource::Flex,
                     ReceivePresentationSurface::Audio) != 0
        || sync.status() != ReceiveSyncStatus::Off) {
        return fail("disabled sync should not delay presentation");
    }

    ReceivePresentationSettings settings;
    settings.enabled = true;
    settings.baseLatencyMs = 250;
    settings.manualOffsetMs = 120;
    sync.setSettings(settings);
    if (sync.status() != ReceiveSyncStatus::Manual
        || sync.delayMs(ReceivePresentationSource::Flex,
                        ReceivePresentationSurface::Waterfall) != 370
        || sync.delayMs(ReceivePresentationSource::KiwiSdr,
                        ReceivePresentationSurface::Waterfall) != 250) {
        return fail("positive manual offset should delay Flex");
    }
    if (sync.delayMs(ReceivePresentationSource::Flex,
                     ReceivePresentationSurface::Decoder) != 0
        || sync.delayMs(ReceivePresentationSource::KiwiSdr,
                        ReceivePresentationSurface::Recording) != 0) {
        return fail("decoder and recording surfaces should stay raw");
    }

    sync.setManualOffsetMs(-80);
    if (sync.delayMs(ReceivePresentationSource::Flex,
                     ReceivePresentationSurface::Meter) != 250
        || sync.delayMs(ReceivePresentationSource::KiwiSdr,
                        ReceivePresentationSurface::Meter) != 330) {
        return fail("negative manual offset should delay KiwiSDR");
    }

    sync.setMode(ReceiveSyncMode::AutoAssist);
    sync.setAutoEstimate(ReceiveSyncEstimate{.offsetMs = 640,
                                             .confidence = 0.90f,
                                             .valid = true});
    if (sync.status() != ReceiveSyncStatus::Locked
        || sync.delayMs(ReceivePresentationSource::Flex,
                        ReceivePresentationSurface::Audio) != 890
        || sync.delayMs(ReceivePresentationSource::KiwiSdr,
                        ReceivePresentationSurface::Audio) != 250) {
        return fail("locked auto estimate should replace manual offset");
    }

    sync.setAutoEstimate(ReceiveSyncEstimate{.offsetMs = 640,
                                             .confidence = 0.10f,
                                             .valid = true});
    if (sync.status() != ReceiveSyncStatus::LowConfidence
        || sync.delayMs(ReceivePresentationSource::KiwiSdr,
                        ReceivePresentationSurface::Audio) != 330) {
        return fail("low-confidence auto estimate should fall back to manual");
    }

    sync.setAutoEstimate(ReceiveSyncEstimate{.offsetMs = 640,
                                             .confidence = 0.10f,
                                             .valid = true,
                                             .driftPpm = 0,
                                             .held = true});
    if (sync.status() != ReceiveSyncStatus::Holding
        || sync.delayMs(ReceivePresentationSource::Flex,
                        ReceivePresentationSurface::Audio) != 890
        || sync.delayMs(ReceivePresentationSource::KiwiSdr,
                        ReceivePresentationSurface::Audio) != 250) {
        return fail("held auto lock should continue applying the last offset");
    }

    if (ReceivePresentationSync::adjustedAutoOffsetMs(500, 510, false) != 500) {
        return fail("near accepted auto estimates should stay inside the deadband");
    }
    if (ReceivePresentationSync::adjustedAutoOffsetMs(500, 620, false) != 520) {
        return fail("near-lock auto estimates should apply a bounded positive step");
    }
    if (ReceivePresentationSync::adjustedAutoOffsetMs(500, 360, false) != 480) {
        return fail("near-lock auto estimates should apply a bounded negative step");
    }
    if (ReceivePresentationSync::adjustedAutoOffsetMs(500, 900, true) != 900) {
        return fail("new locks should apply the candidate offset");
    }

    const QVector<ReceiveSyncAudioPairEndpoint> oneFlex{
        ReceiveSyncAudioPairEndpoint{.sliceId = 1,
                                     .frequencyHz = 14197000}};
    const QVector<ReceiveSyncAudioPairEndpoint> oneMatchingOneUnmatchedKiwi{
        ReceiveSyncAudioPairEndpoint{.sliceId = 2,
                                     .frequencyHz = 14197000,
                                     .kiwiProfileId = QStringLiteral("kiwi-a")},
        ReceiveSyncAudioPairEndpoint{.sliceId = 3,
                                     .frequencyHz = 14200000,
                                     .kiwiProfileId = QStringLiteral("kiwi-b")}};
    const ReceiveSyncAudioPairSelection ignoredUnmatched =
        selectReceiveSyncAudioPair(oneFlex, oneMatchingOneUnmatchedKiwi, 50);
    if (!ignoredUnmatched.usable()
        || ignoredUnmatched.kiwiProfileId != QStringLiteral("kiwi-a")
        || ignoredUnmatched.audibleKiwiCount != 2
        || ignoredUnmatched.matchingPairCount != 1) {
        return fail("pair selector should ignore unmatched Kiwi audio sources");
    }

    const QVector<ReceiveSyncAudioPairEndpoint> twoMatchingKiwis{
        ReceiveSyncAudioPairEndpoint{.sliceId = 2,
                                     .frequencyHz = 14197000,
                                     .kiwiProfileId = QStringLiteral("kiwi-a")},
        ReceiveSyncAudioPairEndpoint{.sliceId = 3,
                                     .frequencyHz = 14197020,
                                     .kiwiProfileId = QStringLiteral("kiwi-b")}};
    const ReceiveSyncAudioPairSelection ambiguousKiwis =
        selectReceiveSyncAudioPair(oneFlex, twoMatchingKiwis, 50);
    if (!ambiguousKiwis.ambiguous()
        || ambiguousKiwis.reason != QStringLiteral("multipleFrequencyMatches")) {
        return fail("pair selector should reject multiple matching Kiwi sources");
    }

    const QVector<ReceiveSyncAudioPairEndpoint> twoFlex{
        ReceiveSyncAudioPairEndpoint{.sliceId = 1,
                                     .frequencyHz = 14197000},
        ReceiveSyncAudioPairEndpoint{.sliceId = 4,
                                     .frequencyHz = 14250000}};
    const ReceiveSyncAudioPairSelection ambiguousFlex =
        selectReceiveSyncAudioPair(twoFlex, oneMatchingOneUnmatchedKiwi, 50);
    if (!ambiguousFlex.ambiguous()
        || ambiguousFlex.reason != QStringLiteral("multipleAudibleFlex")) {
        return fail("pair selector should reject mixed Flex audio");
    }

    if (receivePresentationVisualQueueKey(
            ReceivePresentationSource::Flex,
            ReceivePresentationSurface::Waterfall,
            QStringLiteral("0x21"))
            != QStringLiteral("flex:waterfall:0x21")
        || receivePresentationVisualQueueKey(
               ReceivePresentationSource::KiwiSdr,
               ReceivePresentationSurface::Waterfall,
               QStringLiteral("kiwi-a"))
               != QStringLiteral("kiwi:waterfall:kiwi-a")
        || receivePresentationVisualQueueKey(
               ReceivePresentationSource::KiwiSdr,
               ReceivePresentationSurface::Meter,
               QStringLiteral("  "))
               != QStringLiteral("kiwi:meter")) {
        return fail("visual queue keys should include source-specific ids");
    }
    if (receivePresentationVisualQueueKey(
            ReceivePresentationSource::KiwiSdr,
            ReceivePresentationSurface::Waterfall,
            QStringLiteral("kiwi-a"))
        == receivePresentationVisualQueueKey(
            ReceivePresentationSource::KiwiSdr,
            ReceivePresentationSurface::Waterfall,
            QStringLiteral("kiwi-b"))) {
        return fail("different Kiwi profiles should use different visual queues");
    }

    if (receivePresentationExternalKiwiDelayMs(
            QStringLiteral("kiwi-a"), QStringLiteral("kiwi-a"), 730)
            != 730
        || receivePresentationExternalKiwiDelayMs(
               QStringLiteral("kiwi-b"), QStringLiteral("kiwi-a"), 730)
               != 0
        || receivePresentationExternalKiwiDelayMs(
               QStringLiteral("kiwi-a"), QStringLiteral("kiwi-b"), 730)
               != 0
        || receivePresentationExternalKiwiDelayMs(
               QStringLiteral("kiwi-b"), QStringLiteral("kiwi-b"), 730)
               != 730) {
        return fail("only the matched Kiwi profile should receive sync delay");
    }
    if (receivePresentationExternalKiwiDelayMs(
            QStringLiteral("kiwi-a"), QString(), 730)
            != 730
        || receivePresentationExternalKiwiDelayMs(
               QStringLiteral("kiwi-b"), QString(), 730)
               != 730) {
        return fail("untargeted Kiwi delay should apply to every external Kiwi profile");
    }
    if (receivePresentationExternalKiwiDelayMs(
            QStringLiteral("kiwi-a"), QStringLiteral("kiwi-a"), -20)
        != 0) {
        return fail("external Kiwi delay routing should clamp negative delays");
    }

    if (!receivePresentationShouldPrebufferAfterDelayChange(120, 360, true,
                                                            false)
        || receivePresentationShouldPrebufferAfterDelayChange(120, 360, true,
                                                             true)
        || receivePresentationShouldPrebufferAfterDelayChange(360, 120, true,
                                                             false)
        || receivePresentationShouldPrebufferAfterDelayChange(120, 360, false,
                                                             false)) {
        return fail("delay growth should prebuffer only empty audible sources");
    }

    ReceivePresentationQueue<QByteArray> queue;
    queue.enqueue({.source = ReceivePresentationSource::Flex,
                   .surface = ReceivePresentationSurface::Audio,
                   .sourceId = QStringLiteral("flex"),
                   .arrivalMs = 1000,
                   .dueMs = 1300,
                   .sequence = 2,
                   .payload = QByteArray("late")});
    queue.enqueue({.source = ReceivePresentationSource::KiwiSdr,
                   .surface = ReceivePresentationSurface::Meter,
                   .sourceId = QStringLiteral("kiwi"),
                   .arrivalMs = 1000,
                   .dueMs = 1200,
                   .sequence = 1,
                   .payload = QByteArray("early")});
    if (queue.releaseDue(1199).size() != 0) {
        return fail("queue should hold items before their due time");
    }
    const QVector<ReceivePresentationQueuedItem<QByteArray>> first =
        queue.releaseDue(1200);
    if (first.size() != 1 || first[0].payload != QByteArray("early")) {
        return fail("queue should release earliest due item first");
    }
    const QVector<ReceivePresentationQueuedItem<QByteArray>> second =
        queue.releaseDue(1400);
    if (second.size() != 1 || second[0].payload != QByteArray("late")) {
        return fail("queue should release remaining due items");
    }

    ReceivePresentationQueue<QByteArray> orderedQueue;
    orderedQueue.enqueue({.source = ReceivePresentationSource::Flex,
                          .surface = ReceivePresentationSurface::Waterfall,
                          .sourceId = QStringLiteral("flex:waterfall"),
                          .arrivalMs = 1000,
                          .dueMs = 1200,
                          .sequence = 2,
                          .payload = QByteArray("second")});
    orderedQueue.enqueue({.source = ReceivePresentationSource::Flex,
                          .surface = ReceivePresentationSurface::Waterfall,
                          .sourceId = QStringLiteral("flex:waterfall"),
                          .arrivalMs = 999,
                          .dueMs = 1200,
                          .sequence = 1,
                          .payload = QByteArray("first")});
    orderedQueue.enqueue({.source = ReceivePresentationSource::Flex,
                          .surface = ReceivePresentationSurface::Waterfall,
                          .sourceId = QStringLiteral("flex:waterfall"),
                          .arrivalMs = 1001,
                          .dueMs = 1200,
                          .sequence = 3,
                          .payload = QByteArray("third")});
    const QVector<ReceivePresentationQueuedItem<QByteArray>> orderedFirst =
        orderedQueue.releaseDue(1200, 2);
    if (orderedFirst.size() != 2
        || orderedFirst[0].payload != QByteArray("first")
        || orderedFirst[1].payload != QByteArray("second")) {
        return fail("queue should preserve sequence for equal due times");
    }
    if (orderedQueue.size() != 1) {
        return fail("limited queue release should leave remaining due items queued");
    }
    const QVector<ReceivePresentationQueuedItem<QByteArray>> orderedSecond =
        orderedQueue.releaseDue(1200);
    if (orderedSecond.size() != 1
        || orderedSecond[0].payload != QByteArray("third")) {
        return fail("queue should release remaining equal-due item in order");
    }

    ReceivePresentationQueue<QByteArray> trimmedQueue;
    trimmedQueue.enqueue({.dueMs = 1000, .sequence = 1,
                          .payload = QByteArray("drop")});
    trimmedQueue.enqueue({.dueMs = 1001, .sequence = 2,
                          .payload = QByteArray("keep-a")});
    trimmedQueue.enqueue({.dueMs = 1002, .sequence = 3,
                          .payload = QByteArray("keep-b")});
    trimmedQueue.trimToSize(2);
    const QVector<ReceivePresentationQueuedItem<QByteArray>> trimmed =
        trimmedQueue.releaseDue(2000);
    if (trimmed.size() != 2
        || trimmed[0].payload != QByteArray("keep-a")
        || trimmed[1].payload != QByteArray("keep-b")) {
        return fail("queue trim should drop oldest pending visual items");
    }

    ReceivePresentationQueue<QByteArray> sourceQueue;
    const QString kiwiAKey = receivePresentationVisualQueueKey(
        ReceivePresentationSource::KiwiSdr,
        ReceivePresentationSurface::Waterfall,
        QStringLiteral("kiwi-a"));
    const QString kiwiBKey = receivePresentationVisualQueueKey(
        ReceivePresentationSource::KiwiSdr,
        ReceivePresentationSurface::Waterfall,
        QStringLiteral("kiwi-b"));
    sourceQueue.enqueue({.source = ReceivePresentationSource::KiwiSdr,
                         .surface = ReceivePresentationSurface::Waterfall,
                         .sourceId = kiwiAKey,
                         .dueMs = 1000,
                         .sequence = 1,
                         .payload = QByteArray("kiwi-a")});
    sourceQueue.enqueue({.source = ReceivePresentationSource::KiwiSdr,
                         .surface = ReceivePresentationSurface::Waterfall,
                         .sourceId = kiwiBKey,
                         .dueMs = 1001,
                         .sequence = 2,
                         .payload = QByteArray("kiwi-b")});
    sourceQueue.enqueue({.source = ReceivePresentationSource::Flex,
                         .surface = ReceivePresentationSurface::Waterfall,
                         .sourceId = QStringLiteral("flex:waterfall:0x21"),
                         .dueMs = 1002,
                         .sequence = 3,
                         .payload = QByteArray("flex")});
    const qsizetype removed = sourceQueue.removeIf(
        [&](const ReceivePresentationQueuedItem<QByteArray>& item) {
            return item.source == ReceivePresentationSource::KiwiSdr
                   && item.sourceId == kiwiAKey;
        });
    const QVector<ReceivePresentationQueuedItem<QByteArray>> remainingSources =
        sourceQueue.releaseDue(2000);
    if (removed != 1 || remainingSources.size() != 2
        || remainingSources[0].payload != QByteArray("kiwi-b")
        || remainingSources[1].payload != QByteArray("flex")) {
        return fail("source-specific visual queue clear should keep other sources");
    }

    constexpr int sampleRate = 1000;
    constexpr int delayMs = 120;
    const QVector<float> flex = syntheticSignal(5000);
    QVector<float> kiwi(flex.size(), 0.0f);
    const int delaySamples = delayMs * sampleRate / 1000;
    for (int i = 0; i + delaySamples < flex.size(); ++i) {
        kiwi[i + delaySamples] = flex[i];
    }

    ReceiveAudioDelayEstimator estimator(
        ReceiveAudioDelayEstimator::Config{.sampleRateHz = sampleRate,
                                           .maxOffsetMs = 300,
                                           .minOverlapMs = 150,
                                           .minPeakCorrelation = 0.50f,
                                           .minConfidence = 0.10f});
    const ReceiveAudioDelayEstimate estimate = estimator.estimate(flex, kiwi);
    if (!estimate.valid || std::abs(estimate.offsetMs - delayMs) > 2) {
        return fail("audio estimator should recover a known positive delay");
    }

    QVector<float> flexEnvelopeSignal;
    QVector<float> kiwiEnvelopeSignal(flex.size(), 0.0f);
    flexEnvelopeSignal.reserve(flex.size());
    for (int i = 0; i < flex.size(); ++i) {
        const float env = syntheticEnvelopeAt(i);
        flexEnvelopeSignal.append(
            env * std::sin(static_cast<float>(i) * 0.071f));
    }
    for (int i = 0; i + delaySamples < flexEnvelopeSignal.size(); ++i) {
        const float env = syntheticEnvelopeAt(i);
        kiwiEnvelopeSignal[i + delaySamples] =
            env * std::sin(static_cast<float>(i) * 0.113f + 0.7f);
    }
    ReceiveAudioDelayEstimator envelopeEstimator(
        ReceiveAudioDelayEstimator::Config{.sampleRateHz = sampleRate,
                                           .maxOffsetMs = 300,
                                           .minOverlapMs = 150,
                                           .minPeakCorrelation = 0.08f,
                                           .minConfidence = 0.05f});
    const ReceiveAudioDelayEstimate envelopeEstimate =
        envelopeEstimator.estimate(flexEnvelopeSignal, kiwiEnvelopeSignal);
    if (!envelopeEstimate.valid
        || std::abs(envelopeEstimate.offsetMs - delayMs) > 15) {
        return fail("audio estimator should recover envelope-matched delay");
    }

    QVector<float> unrelatedCarrier(flex.size(), 0.0f);
    for (int i = 0; i < unrelatedCarrier.size(); ++i) {
        unrelatedCarrier[i] =
            std::sin(static_cast<float>(i) * 0.113f + 0.7f);
    }
    const ReceiveAudioDelayEstimate unrelatedEstimate =
        envelopeEstimator.estimate(flexEnvelopeSignal, unrelatedCarrier);
    if (unrelatedEstimate.confidence >= 0.18f
        && unrelatedEstimate.peakCorrelation >= 0.20f) {
        return fail("carrier-only mismatch should not be stable-lock eligible");
    }

    return 0;
}
