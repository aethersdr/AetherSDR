#include "core/PanadapterStream.h"
#include "gui/DbmRangeTransition.h"

#include <QCoreApplication>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    PanadapterStream stream;
    constexpr quint32 kStreamId = 0x40000000;

    CHECK(!stream.cancelPendingDbmRange(kStreamId));

    // A matching radio echo consumes the pending request normally.
    stream.setDbmRange(kStreamId, -135.0f, -40.0f, true);
    stream.setDbmRange(kStreamId, -135.0f, -40.0f);
    CHECK(!stream.cancelPendingDbmRange(kStreamId));

    // A different radio range is held during the stale-echo window.
    stream.setDbmRange(kStreamId, -138.0f, -95.0f, true);
    stream.setDbmRange(kStreamId, -130.0f, -30.0f);
    CHECK(stream.cancelPendingDbmRange(kStreamId));
    CHECK(!stream.cancelPendingDbmRange(kStreamId));

    // Once explicitly cancelled for a band change, that same authoritative
    // range applies normally instead of remaining blocked behind the old drag.
    stream.setDbmRange(kStreamId, -130.0f, -30.0f);
    CHECK(!stream.cancelPendingDbmRange(kStreamId));

    // A single radio-authoritative mismatch must survive until the timeout.
    // PanadapterModel emits levelChanged only when the values change, so there
    // may be no second status available to repair the decoder afterward.
    DbmRangeTransition::Handshake handshake;
    const quint64 mismatchGeneration = handshake.arm(-138.0f, -95.0f, 1000);
    const DbmRangeTransition::HandshakeDecision heldMismatch =
        handshake.observeRadioRange(-130.0f, -30.0f, 1100, 2000);
    CHECK(heldMismatch.action
          == DbmRangeTransition::HandshakeAction::HoldRequestedRange);
    CHECK(std::abs(heldMismatch.range.minDbm - -138.0f) < 0.01f);
    CHECK(std::abs(heldMismatch.range.maxDbm - -95.0f) < 0.01f);
    const DbmRangeTransition::HandshakeDecision mismatchTimeout =
        handshake.finish(mismatchGeneration);
    CHECK(mismatchTimeout.action
          == DbmRangeTransition::HandshakeAction::ReconcileRadioRange);
    CHECK(std::abs(mismatchTimeout.range.minDbm - -130.0f) < 0.01f);
    CHECK(std::abs(mismatchTimeout.range.maxDbm - -30.0f) < 0.01f);
    CHECK(!handshake.active());

    // A matching echo completes immediately and invalidates its timer.
    const quint64 matchingGeneration = handshake.arm(-140.0f, -40.0f, 2000);
    const DbmRangeTransition::HandshakeDecision matchingEcho =
        handshake.observeRadioRange(-140.0f, -40.0f, 2100, 2000);
    CHECK(matchingEcho.action
          == DbmRangeTransition::HandshakeAction::ApplyRadioRange);
    CHECK(handshake.finish(matchingGeneration).action
          == DbmRangeTransition::HandshakeAction::Ignore);

    // If firmware accepts the command without echoing a range, retire only the
    // guard and keep the decoder on the requested range.
    const quint64 noEchoGeneration = handshake.arm(-135.0f, -45.0f, 3000);
    const DbmRangeTransition::HandshakeDecision noEchoTimeout =
        handshake.finish(noEchoGeneration);
    CHECK(noEchoTimeout.action
          == DbmRangeTransition::HandshakeAction::RetireWithoutEcho);
    CHECK(std::abs(noEchoTimeout.range.minDbm - -135.0f) < 0.01f);
    CHECK(std::abs(noEchoTimeout.range.maxDbm - -45.0f) < 0.01f);

    // A newer request makes the previous timer harmless, and a band restore
    // immediately yields to the current radio-owned range.
    const quint64 staleGeneration = handshake.arm(-132.0f, -42.0f, 4000);
    const quint64 currentGeneration = handshake.arm(-128.0f, -38.0f, 4100);
    CHECK(handshake.finish(staleGeneration).action
          == DbmRangeTransition::HandshakeAction::Ignore);
    const DbmRangeTransition::HandshakeDecision bandRestore =
        handshake.cancelForRadioAuthority(-125.0f, -25.0f);
    CHECK(bandRestore.action
          == DbmRangeTransition::HandshakeAction::ReconcileRadioRange);
    CHECK(std::abs(bandRestore.range.minDbm - -125.0f) < 0.01f);
    CHECK(std::abs(bandRestore.range.maxDbm - -25.0f) < 0.01f);
    CHECK(handshake.finish(currentGeneration).action
          == DbmRangeTransition::HandshakeAction::Ignore);

    // If multiple authoritative ranges arrive, reconciliation uses the latest.
    const quint64 latestGeneration = handshake.arm(-136.0f, -46.0f, 5000);
    handshake.observeRadioRange(-130.0f, -30.0f, 5100, 2000);
    handshake.observeRadioRange(-126.0f, -26.0f, 5200, 2000);
    const DbmRangeTransition::HandshakeDecision latestTimeout =
        handshake.finish(latestGeneration);
    CHECK(latestTimeout.action
          == DbmRangeTransition::HandshakeAction::ReconcileRadioRange);
    CHECK(std::abs(latestTimeout.range.minDbm - -126.0f) < 0.01f);
    CHECK(std::abs(latestTimeout.range.maxDbm - -26.0f) < 0.01f);

    // During a dBm-range handshake, the decoder may already use the new range
    // while the radio is still encoding FFT pixels with the old range. Detect
    // and undo that temporary reinterpretation, then stop rebasing as soon as
    // the radio begins using the new encoding.
    const QVector<float> previousBins{-120.0f, -115.0f, -110.0f,
                                      -105.0f, -100.0f};
    constexpr float kOldMinDbm = -180.0f;
    constexpr float kOldMaxDbm = -85.0f;
    constexpr float kNewMinDbm = -180.0f;
    constexpr float kNewMaxDbm = -95.0f;
    QVector<float> oldEncodedBinsDecodedWithNewRange;
    oldEncodedBinsDecodedWithNewRange.reserve(previousBins.size());
    for (const float bin : previousBins) {
        const float fraction = (kOldMaxDbm - bin) / (kOldMaxDbm - kOldMinDbm);
        oldEncodedBinsDecodedWithNewRange.append(
            kNewMaxDbm - fraction * (kNewMaxDbm - kNewMinDbm));
    }

    const DbmRangeTransition::Evaluation staleEncoding =
        DbmRangeTransition::evaluate(oldEncodedBinsDecodedWithNewRange,
                                     previousBins,
                                     kOldMinDbm, kOldMaxDbm,
                                     kNewMinDbm, kNewMaxDbm);
    CHECK(staleEncoding.useRebasedBins);
    CHECK(!staleEncoding.newEncodingObserved);
    CHECK(staleEncoding.rebasedBins.size() == previousBins.size());
    for (int i = 0; i < previousBins.size(); ++i) {
        CHECK(std::abs(staleEncoding.rebasedBins[i] - previousBins[i]) < 0.01f);
    }

    const DbmRangeTransition::Evaluation newEncoding =
        DbmRangeTransition::evaluate(previousBins, previousBins,
                                     kOldMinDbm, kOldMaxDbm,
                                     kNewMinDbm, kNewMaxDbm);
    CHECK(!newEncoding.useRebasedBins);
    CHECK(newEncoding.newEncodingObserved);

    // In 3D, the visible axis is anchored to the measured DSS floor rather
    // than the hidden 2D reference level. The radio request must use that same
    // visible range or a deep zoom can place all RF energy outside the encoder
    // aperture and flatten every FFT bin to one endpoint.
    const DbmRangeTransition::Range flex3dRange =
        DbmRangeTransition::manualRequestRange(
            -180.0f, -135.0f, true, -140.0f, 45.0f);
    CHECK(std::abs(flex3dRange.minDbm - -140.0f) < 0.01f);
    CHECK(std::abs(flex3dRange.maxDbm - -95.0f) < 0.01f);
    CHECK(-114.0f > flex3dRange.minDbm && -114.0f < flex3dRange.maxDbm);

    // Narrow 3D ranges must remain visible instead of being pinned at 45 dB.
    // That pin made several arrow clicks appear to do nothing and made a range
    // drag jump only after the hidden value finally crossed the old floor.
    CHECK(std::abs(DbmRangeTransition::displaySpanDb(10.0f) - 10.0f) < 0.01f);
    CHECK(std::abs(DbmRangeTransition::displaySpanDb(20.0f) - 20.0f) < 0.01f);
    CHECK(std::abs(DbmRangeTransition::displaySpanDb(130.0f) - 120.0f) < 0.01f);
    const float onePixelFloorDrag =
        DbmRangeTransition::floorDepthForDrag(6.0f, 240, 239, 480);
    CHECK(onePixelFloorDrag > 6.0f && onePixelFloorDrag < 6.1f);
    CHECK(std::abs(DbmRangeTransition::floorDepthForDrag(
        6.0f, 240, 220, 480) - 7.0f) < 0.01f);
    CHECK(std::abs(DbmRangeTransition::floorDepthForDrag(
        23.0f, 240, -240, 480) - 24.0f) < 0.01f);
    CHECK(std::abs(DbmRangeTransition::floorDepthForDrag(
        1.0f, 240, 720, 480)) < 0.01f);
    const float fractionalFloorDepth =
        DbmRangeTransition::floorDepthFromOffsetDb(-3.390244f);
    CHECK(std::abs(fractionalFloorDepth - 3.390244f) < 0.0001f);
    CHECK(std::abs(DbmRangeTransition::floorDepthFromOffsetDb(-30.0f)
                   - 24.0f) < 0.01f);
    CHECK(std::abs(DbmRangeTransition::floorDepthFromOffsetDb(5.0f)) < 0.01f);
    const DbmRangeTransition::Range narrowFlex3dRange =
        DbmRangeTransition::manualRequestRange(
            -124.0f, -114.0f, true, -118.5f, 10.0f);
    CHECK(std::abs(narrowFlex3dRange.minDbm - -118.5f) < 0.01f);
    CHECK(std::abs(narrowFlex3dRange.maxDbm - -108.5f) < 0.01f);

    // A 3D floor drag is previewed locally, then its release shifts the radio
    // encoder aperture once so fresh rows recover detail below the old floor.
    const DbmRangeTransition::Range movedFloorRange =
        DbmRangeTransition::manualRequestRange(
            -118.5f, -108.5f, true, -123.5f, 10.0f);
    CHECK(std::abs(movedFloorRange.minDbm - -123.5f) < 0.01f);
    CHECK(std::abs(movedFloorRange.maxDbm - -113.5f) < 0.01f);
    CHECK(DbmRangeTransition::materiallyDifferent(
        narrowFlex3dRange, movedFloorRange));
    CHECK(!DbmRangeTransition::materiallyDifferent(
        movedFloorRange, {-123.48f, -113.48f}));

    const DbmRangeTransition::Range flex2dRange =
        DbmRangeTransition::manualRequestRange(
            -180.0f, -135.0f, false, -140.0f, 45.0f);
    CHECK(std::abs(flex2dRange.minDbm - -180.0f) < 0.01f);
    CHECK(std::abs(flex2dRange.maxDbm - -135.0f) < 0.01f);

    if (g_failures == 0) {
        std::printf("panadapter_dbm_range_test: all checks passed\n");
        return 0;
    }
    std::printf("panadapter_dbm_range_test: %d failure(s)\n", g_failures);
    return 1;
}
