#include "core/backends/flex/PanadapterStream.h"
#include "core/VitaBinCoverage.h"
#include "gui/DbmRangeTransition.h"
#include "models/RadioModel.h"
#include "models/PanadapterModel.h"
#include "TestSettingsProfile.h"

#include <QCoreApplication>
#include <QtEndian>

#include <cstdio>

using namespace AetherSDR;

namespace AetherSDR {
struct PanadapterDbmRangeTestAccess {
    static void fft(PanadapterStream& stream, quint32 id) {
        QByteArray frame(48, '\0');
        auto* raw = reinterpret_cast<uchar*>(frame.data());
        qToBigEndian<quint16>(4, raw + 30);
        qToBigEndian<quint16>(2, raw + 32);
        qToBigEndian<quint16>(4, raw + 34);
        qToBigEndian<quint32>(1, raw + 36);
        for (int i = 0; i < 4; ++i) {
            qToBigEndian<quint16>(350, raw + 40 + 2 * i);
        }
        stream.decodeFFT(raw, frame.size(), false, id);
    }
};
}

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("panadapter-dbm-range"));
    QCoreApplication app(argc, argv);

    {
        RadioModel radio;
        const QString panId = QStringLiteral("0x40000011");
        radio.handleStatusForTest(QStringLiteral("display pan ") + panId,
            {{"client_handle", "0x0"}, {"min_dbm", "-130"}, {"max_dbm", "-40"}});
        radio.handleStatusForTest(QStringLiteral("display pan ") + panId,
            {{"client_handle", "0xdeadbeef"}, {"min_dbm", "-130"}, {"max_dbm", "-40"}});
        const PanadapterModel* foreign = radio.panadapter(panId);
        CHECK(foreign && !foreign->ownedByClient(radio.ourClientHandle()));
        bool replied = false;
        CHECK(!radio.sendCmdPublic(QStringLiteral("display pan set ") + panId
                + QStringLiteral(" min_dbm=-154 max_dbm=-40"),
            [&](int, const QString&) { replied = true; }));
        CHECK(!replied);
        CHECK(!radio.sendCommand(QStringLiteral("display pan set ") + panId
                + QStringLiteral(" min_dbm=-154 max_dbm=-40")));
    }

    int dispatches = 0;
    int commits = 0;
    const auto dispatch = [&]() { ++dispatches; return true; };
    const auto commit = [&]() { ++commits; };
    CHECK(!DbmRangeTransition::dispatchValidatedRange(
        {-202.0f, -112.0f}, dispatch, commit));
    CHECK(!DbmRangeTransition::dispatchValidatedRange(
        {std::nanf(""), -40.0f}, dispatch, commit));
    CHECK(dispatches == 0 && commits == 0);
    CHECK(!DbmRangeTransition::dispatchValidatedRange(
        {-135.0f, -40.0f}, []() { return false; }, commit));
    CHECK(commits == 0);
    CHECK(DbmRangeTransition::dispatchValidatedRange(
        {-135.0f, -40.0f}, dispatch, commit));
    CHECK(dispatches == 1 && commits == 1);

    VitaBinCoverage fragmentCoverage;
    fragmentCoverage.reset(8);
    CHECK(fragmentCoverage.markRange(0, 4));
    CHECK(fragmentCoverage.uniqueBins() == 4);
    CHECK(!fragmentCoverage.isComplete());
    CHECK(!fragmentCoverage.markRange(0, 4));
    CHECK(fragmentCoverage.uniqueBins() == 4);
    CHECK(!fragmentCoverage.isComplete());
    CHECK(fragmentCoverage.markRange(2, 4));
    CHECK(fragmentCoverage.uniqueBins() == 6);
    CHECK(!fragmentCoverage.isComplete());
    CHECK(fragmentCoverage.markRange(4, 4));
    CHECK(fragmentCoverage.uniqueBins() == 8);
    CHECK(fragmentCoverage.isComplete());
    fragmentCoverage.reset(10);
    CHECK(fragmentCoverage.uniqueBins() == 0);
    CHECK(fragmentCoverage.totalBins() == 10);
    CHECK(!fragmentCoverage.isComplete());
    CHECK(!fragmentCoverage.markRange(8, 4));

    CHECK(boundedVitaPayloadBinCount(256, 2, 512) == 256);
    CHECK(boundedVitaPayloadBinCount(256, 2, 510) == 255);
    CHECK(boundedVitaPayloadBinCount(256, 2, 1) == 0);
    CHECK(boundedVitaPayloadBinCount(256, 0, 512) == 0);

    QVector<quint16> cleanGrowth(16, 300);
    cleanGrowth.resize(32, 350);
    CHECK(!hasZeroFilledFftGrowthSuffix(cleanGrowth, 16));

    QVector<quint16> zeroFilledGrowth(32, 300);
    std::fill(zeroFilledGrowth.begin() + 16,
              zeroFilledGrowth.end(), quint16{0});
    CHECK(hasZeroFilledFftGrowthSuffix(zeroFilledGrowth, 16));

    QVector<quint16> saturatedFrame(32, 0);
    CHECK(!hasZeroFilledFftGrowthSuffix(saturatedFrame, 16));
    CHECK(!hasZeroFilledFftGrowthSuffix(zeroFilledGrowth, 0));
    CHECK(!hasZeroFilledFftGrowthSuffix(zeroFilledGrowth, 32));

    FftGrowthSuffixGuard growthGuard;
    for (int frame = 0;
         frame < FftGrowthSuffixGuard::kRejectedFramesBeforeFloorFallback;
         ++frame) {
        CHECK(growthGuard.observe(true, 32)
              == FftGrowthSuffixAction::Reject);
    }
    CHECK(growthGuard.observe(true, 32)
          == FftGrowthSuffixAction::EmitWithFloorSuffix);
    CHECK(growthGuard.consecutiveRejectedFrames() == 4);
    CHECK(growthGuard.observe(false, 32)
          == FftGrowthSuffixAction::Accept);
    CHECK(growthGuard.consecutiveRejectedFrames() == 0);
    CHECK(growthGuard.observe(true, 48)
          == FftGrowthSuffixAction::Reject);
    CHECK(growthGuard.consecutiveRejectedFrames() == 1);

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

    // The queued payload owns its decoder aperture even if the GUI changes
    // the stream range before delivering the observation. No socket is used.
    {
        const SpectrumDecodeScale old = stream.setDbmRange(kStreamId, -165, -35);
        CHECK(old.valid());
        SpectrumDecodeScale queued;
        QVector<float> queuedBins;
        QObject receiver;
        QObject::connect(&stream, &PanadapterStream::spectrumReady, &receiver,
            [&](quint32 id, const QVector<float>& bins, qint64,
                const SpectrumDecodeScale& scale) {
                CHECK(id == kStreamId);
                queued = scale;
                queuedBins = bins;
            }, Qt::QueuedConnection);
        PanadapterDbmRangeTestAccess::fft(stream, kStreamId);
        const SpectrumDecodeScale requested = stream.setDbmRange(kStreamId, -127.5f, 2.5f, true);
        CHECK(requested.generation > old.generation);
        const SpectrumDecodeScale staleEcho = stream.setDbmRange(kStreamId, -165, -35);
        CHECK(staleEcho.generation == requested.generation);
        const SpectrumDecodeScale matchingEcho = stream.setDbmRange(kStreamId, -127.5f, 2.5f);
        CHECK(matchingEcho.generation == requested.generation);
        app.processEvents();
        CHECK(queued.generation == old.generation);
        CHECK(queued.minDbm == -165 && queued.maxDbm == -35);
        CHECK(queuedBins.size() == 4);
        CHECK(std::abs(queuedBins.front() - (-35 - 350.0f / 699 * 130)) < 0.001f);
    }

    // A stream that never took a range write (its status matched the -130/-40
    // default) still decodes with a valid generation, and the first real write
    // supersedes it.
    {
        constexpr quint32 kFreshStreamId = kStreamId + 1;
        stream.registerPanStream(kFreshStreamId);
        SpectrumDecodeScale first;
        QObject receiver;
        QObject::connect(&stream, &PanadapterStream::spectrumReady, &receiver,
            [&](quint32 id, const QVector<float>&, qint64, const SpectrumDecodeScale& scale) {
                if (id == kFreshStreamId) {
                    first = scale;
                }
            }, Qt::QueuedConnection);
        PanadapterDbmRangeTestAccess::fft(stream, kFreshStreamId);
        app.processEvents();
        CHECK(first.valid());
        CHECK(first.minDbm == -130.0f && first.maxDbm == -40.0f);
        const SpectrumDecodeScale written = stream.setDbmRange(kFreshStreamId, -100.0f, -10.0f);
        CHECK(written.generation > first.generation);
        stream.unregisterPanStream(kFreshStreamId);
    }

    {
        DbmRangeTransition::FrameGuard frames;
        const DbmRangeTransition::Range first{-165, -35}, middle{-127.5f, 2.5f};
        const QVector<float> physical(256, -104);
        frames.arm(first, middle, 1000, 2000);
        frames.setTargetScale({middle.minDbm, middle.maxDbm, 2});
        CHECK(!frames.evaluate(physical, physical, {-165, -35, 1}, 1001).newEncodingObserved);
        const QVector<float> wrong(256, -66.5f);
        const auto corrected = frames.evaluate(wrong, physical, {-127.5f, 2.5f, 2}, 1002);
        CHECK(corrected.useRebasedBins && !corrected.newEncodingObserved);
        CHECK(std::abs(corrected.rebasedBins.front() + 104) < 0.001f);
        CHECK(frames.evaluate(physical, physical, {-127.5f, 2.5f, 2}, 1100).newEncodingObserved);
        CHECK(frames.evaluate(wrong, physical, {-127.5f, 2.5f, 2}, 1101).useRebasedBins);
        frames.arm(middle, first, 1200, 2000);
        frames.setTargetScale({-165, -35, 3});
        CHECK(!frames.evaluate(physical, physical, {-165, -35, 1}, 1201).newEncodingObserved);
        const QVector<float> intermediateWire(256, -141.5f);
        const auto reversed = frames.evaluate(intermediateWire, physical, {-165, -35, 3}, 1202);
        CHECK(reversed.useRebasedBins && !reversed.newEncodingObserved);
        CHECK(std::abs(reversed.rebasedBins.front() + 104) < 0.001f);
        for (const DbmRangeTransition::Range intermediate : {
                 DbmRangeTransition::Range{-165, 2.5f}, {-127.5f, -35}}) {
            QVector<float> decoded = physical;
            for (float& bin : decoded) {
                const float fraction = (intermediate.maxDbm - bin)
                    / (intermediate.maxDbm - intermediate.minDbm);
                bin = -35 - fraction * 130;
            }
            const auto partial = frames.evaluate(decoded, physical, {-165, -35, 3}, 1250);
            CHECK(partial.useRebasedBins && !partial.newEncodingObserved);
            CHECK(std::abs(partial.rebasedBins.front() + 104) < 0.001f);
        }
        CHECK(!frames.evaluate(wrong, physical, {-127.5f, 2.5f, 2}, 3201).useRebasedBins);
        frames.clear();
        CHECK(!frames.evaluate(wrong, physical, {-127.5f, 2.5f, 2}, 1203).useRebasedBins);
    }

    // A single radio-authoritative mismatch must survive until the timeout.
    // PanadapterModel emits levelChanged only when the values change, so there
    // may be no second status available to repair the decoder afterward.
    DbmRangeTransition::Handshake handshake;
    const DbmRangeTransition::Range previousRange{-80.0f, -10.0f};
    const quint64 acceptedGeneration = handshake.arm(-104.0f, -10.0f, 500);
    const DbmRangeTransition::HandshakeDecision acceptedReply =
        handshake.completeReply(acceptedGeneration, true, previousRange);
    CHECK(acceptedReply.action == DbmRangeTransition::HandshakeAction::ReconcileRadioRange);
    CHECK(acceptedReply.range.minDbm == -104.0f && acceptedReply.range.maxDbm == -10.0f);
    CHECK(!handshake.active());
    CHECK(handshake.finish(acceptedGeneration).action == DbmRangeTransition::HandshakeAction::Ignore);
    const quint64 rejectedGeneration = handshake.arm(-104.0f, -10.0f, 600);
    const DbmRangeTransition::HandshakeDecision rejectedReply =
        handshake.completeReply(rejectedGeneration, false, previousRange);
    CHECK(rejectedReply.range.minDbm == -80.0f && rejectedReply.range.maxDbm == -10.0f);
    const quint64 supersededGeneration = handshake.arm(-104.0f, -10.0f, 700);
    const quint64 supersedingGeneration = handshake.arm(-128.0f, -10.0f, 750);
    CHECK(handshake.completeReply(supersededGeneration, true, previousRange).action
          == DbmRangeTransition::HandshakeAction::Ignore);
    handshake.observeRadioRange(-130.0f, -20.0f, 800, 2000);
    const DbmRangeTransition::HandshakeDecision reportedReply =
        handshake.completeReply(supersedingGeneration, true, previousRange);
    // Principle II: status received after the write wins over the accepted request.
    CHECK(reportedReply.range.minDbm == -130.0f && reportedReply.range.maxDbm == -20.0f);
    const DbmRangeTransition::HandshakeDecision laterStatus =
        handshake.observeRadioRange(-130.0f, -20.0f, 850, 2000);
    CHECK(laterStatus.action == DbmRangeTransition::HandshakeAction::ApplyRadioRange);
    const quint64 rejectionAfterStatus = handshake.arm(-128.0f, -10.0f, 900);
    handshake.observeRadioRange(-130.0f, -20.0f, 920, 2000);
    const DbmRangeTransition::HandshakeDecision rejectedAfterStatus =
        handshake.completeReply(rejectionAfterStatus, false, previousRange);
    CHECK(rejectedAfterStatus.range.minDbm == -130.0f && rejectedAfterStatus.range.maxDbm == -20.0f);
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

    // A clipped frame cannot estimate the real floor. Recovery must add
    // lower headroom without sacrificing the existing peak ceiling.
    const DbmRangeTransition::Range clippedRecoveryRange =
        DbmRangeTransition::clippedFloorRecoveryRange(-110.5f, -15.5f);
    CHECK(std::abs(clippedRecoveryRange.minDbm - -116.5f) < 0.01f);
    CHECK(std::abs(clippedRecoveryRange.maxDbm - -15.5f) < 0.01f);
    CHECK(std::abs((clippedRecoveryRange.maxDbm
                    - clippedRecoveryRange.minDbm) - 101.0f) < 0.01f);
    const DbmRangeTransition::Range oneShotRecoveryRange =
        DbmRangeTransition::clippedFloorRecoveryRange(
            -111.0f, -16.0f, 24.0f);
    CHECK(std::abs(oneShotRecoveryRange.minDbm - -135.0f) < 0.01f);
    CHECK(std::abs(oneShotRecoveryRange.maxDbm - -16.0f) < 0.01f);

    const DbmRangeTransition::Range boundedFloor =
        DbmRangeTransition::clippedFloorRecoveryRange(-178.0f, -88.0f, 24.0f);
    CHECK(boundedFloor.minDbm == -180.0f && boundedFloor.maxDbm == -88.0f);
    CHECK(!DbmRangeTransition::materiallyDifferent(boundedFloor,
        DbmRangeTransition::clippedFloorRecoveryRange(-180.0f, -88.0f, 24.0f)));
    const DbmRangeTransition::Range expandedPeak =
        DbmRangeTransition::clippedPeakRecoveryRange(-178.0f, -88.0f, 24.0f);
    CHECK(expandedPeak.minDbm == -178.0f && expandedPeak.maxDbm == -64.0f);
    const DbmRangeTransition::Range spanLimited =
        DbmRangeTransition::clippedPeakRecoveryRange(-180.0f, -10.0f, 24.0f);
    CHECK(spanLimited.minDbm == -180.0f && spanLimited.maxDbm == 0.0f);
    const DbmRangeTransition::Range ceilingLimited =
        DbmRangeTransition::clippedPeakRecoveryRange(-130.0f, 10.0f, 24.0f);
    CHECK(ceilingLimited.minDbm == -130.0f && ceilingLimited.maxDbm == 20.0f);
    CHECK(!DbmRangeTransition::materiallyDifferent({-180.0f, 0.0f},
        DbmRangeTransition::clippedFloorRecoveryRange(-180.0f, 0.0f, 24.0f)));

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
