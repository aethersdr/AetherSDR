// radiocert must not report a reading it never took (#7083, #7084).
//
// Socket-free: the stages run against a RadioModel with a backend installed for
// test and an AudioEngine that is constructed and never started. No
// TxController is given, so every key is refused and nothing can transmit.

#include "core/RadioCertification.h"
#include "core/AudioEngine.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/PcmFrame.h"
#include "models/MeterModel.h"
#include "models/RadioModel.h"
#include "TestSettingsProfile.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QTimer>
#include <QVector>

#include <cstdio>
#include <memory>
#include <optional>

namespace AetherSDR {

class RadioCertificationTestAccess {
public:
    static QJsonArray stages(const RadioCertification& cert) { return cert.m_stages; }
    static QJsonObject meterSnapshot(const RadioCertification& cert)
    {
        return cert.meterSnapshot();
    }
    static void rxSidebands(RadioCertification& cert, const RadioCertification::Options& o)
    {
        cert.stageRxSidebands(o);
    }
    static void sideband(RadioCertification& cert, const RadioCertification::Options& o)
    {
        cert.stageSideband(o);
    }
    static void lifecycle(RadioCertification& cert, const RadioCertification::Options& o)
    {
        cert.stageLifecycle(o);
    }
    static int keyRefusals(const RadioCertification& cert) { return cert.m_keyRefusals; }
    static void meterScale(RadioCertification& cert, const RadioCertification::Options& o)
    {
        cert.stageMeterScale(o);
    }
    static RadioCertification::MeterScaleReading judgeMeterScale(
        bool keyedOk, const QJsonObject& keyed, double tone)
    {
        return RadioCertification::judgeMeterScale(keyedOk, keyed, tone);
    }
};

}  // namespace AetherSDR

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    std::fflush(stdout);
    failures += !condition;
}

// The slice fixture installs only while disconnected, so the link is a flag.
class IdleBackend final : public hl2::Hl2Backend {
public:
    bool linked{true};
    bool isConnected() const override { return linked; }
};

QJsonObject stageNamed(const RadioCertification& cert, const QString& id)
{
    for (const QJsonValue& v : RadioCertificationTestAccess::stages(cert)) {
        if (v.toObject().value(QStringLiteral("id")).toString() == id)
            return v.toObject();
    }
    return {};
}

void defineMeter(MeterModel& meters, int idx, const char* src, const char* name,
                 const char* unit)
{
    MeterDef d;
    d.index = idx;
    d.source = QString::fromLatin1(src);
    d.name = QString::fromLatin1(name);
    d.unit = QString::fromLatin1(unit);
    d.low = -200.0;
    d.high = 300.0;
    meters.defineMeter(d);
}

void testMeterScaleJudgement()
{
    using Access = RadioCertificationTestAccess;

    // THE REGRESSION: the HL2 runs from #7084, a leftover 255.99 marked stale.
    {
        const QJsonObject keyed{
            {QStringLiteral("micPeakDbfs"), -20.1},
            {QStringLiteral("swr"), 255.9921875},
            {QStringLiteral("swrStale"), true},
        };
        const auto r = Access::judgeMeterScale(true, keyed, -20.0);
        const QString problems = r.problems.join(QStringLiteral("; "));
        check(!problems.contains(QLatin1String("saturated")),
              "meter-scale: a stale SWR does not raise the saturation concern");
        check(r.swr.isNull(), "meter-scale: a stale SWR is published as null, not the leftover");
        check(r.swrNote.contains(QLatin1String("not measurable")),
              "meter-scale: a stale SWR says it was not measurable");
        check(r.problems.isEmpty(), "meter-scale: and a correct mic peak leaves no concern");
    }
    // Positive control: a FRESH 255.99 still reaches the guard.
    {
        const QJsonObject keyed{
            {QStringLiteral("micPeakDbfs"), -20.1},
            {QStringLiteral("swr"), 255.9921875},
        };
        const auto r = Access::judgeMeterScale(true, keyed, -20.0);
        check(r.problems.join(QStringLiteral("; ")).contains(QLatin1String("saturated")),
              "meter-scale: a fresh 255.99 still raises the saturation concern");
        check(r.swr.isDouble() && r.swr.toDouble() > 255.0,
              "meter-scale: and a fresh SWR is published as its value");
    }
    // A meter that never published at all reads the same as a stale one.
    {
        const auto r = Access::judgeMeterScale(
            true, QJsonObject{{QStringLiteral("micPeakDbfs"), -20.0}}, -20.0);
        check(r.swr.isNull() && r.problems.isEmpty(),
              "meter-scale: an absent SWR is null and not a concern");
    }
    // The same freshness rule for the mic peak.
    {
        const QJsonObject keyed{
            {QStringLiteral("micPeakDbfs"), -40.0},
            {QStringLiteral("micPeakDbfsStale"), true},
            {QStringLiteral("swr"), 1.1},
        };
        const auto r = Access::judgeMeterScale(true, keyed, -20.0);
        const QString problems = r.problems.join(QStringLiteral("; "));
        check(!problems.contains(QLatin1String("scale error")),
              "meter-scale: a stale mic peak is not judged as a scale error");
        check(problems.contains(QLatin1String("no fresh mic peak")),
              "meter-scale: a stale mic peak is reported as no fresh reading");
        check(r.micPeakDbfs.isNull(), "meter-scale: and is published as null");
    }
    {
        const QJsonObject keyed{{QStringLiteral("micPeakDbfs"), -40.0}};
        const auto r = Access::judgeMeterScale(true, keyed, -20.0);
        check(r.problems.join(QStringLiteral("; ")).contains(QLatin1String("scale error")),
              "meter-scale: a fresh mic peak 20 dB off is still a scale error");
    }
    // A refused key measured nothing, whatever the snapshot holds.
    {
        const QJsonObject keyed{
            {QStringLiteral("micPeakDbfs"), -40.0},
            {QStringLiteral("swr"), 255.9921875},
        };
        const auto r = Access::judgeMeterScale(false, keyed, -20.0);
        const QString problems = r.problems.join(QStringLiteral("; "));
        check(problems.contains(QLatin1String("INCONCLUSIVE"))
                  && !problems.contains(QLatin1String("saturated"))
                  && !problems.contains(QLatin1String("scale error")),
              "meter-scale: a refused key is INCONCLUSIVE and judges neither meter");
        check(r.swr.isNull() && r.micPeakDbfs.isNull(),
              "meter-scale: and publishes neither value");
        check(r.swrNote.contains(QLatin1String("not measurable"))
                  && r.swrNote.contains(QLatin1String("refused")),
              "meter-scale: a refused key says the SWR was not measurable, as documented");
    }
}

void testMeterScaleStage()
{
    // The stage end to end: a 255.99 SWR from an earlier key, aged past the
    // snapshot's freshness window, must not appear in the report.
    RadioModel radio;
    std::unique_ptr<IRadioBackend> owned(static_cast<IRadioBackend*>(new IdleBackend()));
    radio.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
    MeterModel& meters = radio.meterModel();
    defineMeter(meters, 1, "TX", "MICPEAK", "dBFS");
    defineMeter(meters, 2, "TX", "SWR", "SWR");
    meters.updateValueByName(QStringLiteral("TX"), QStringLiteral("SWR"), 255.9921875f);
    meters.setLastSwrUpdateMsForTest(QDateTime::currentMSecsSinceEpoch() - 10000);

    AudioEngine audio;   // constructed, never started: no device, no socket
    RadioCertification cert(&radio, &audio);
    const QJsonObject snap = RadioCertificationTestAccess::meterSnapshot(cert);
    check(snap.value(QStringLiteral("swr")).toDouble() > 255.0
              && snap.value(QStringLiteral("swrStale")).toBool(),
          "fixture: the snapshot carries the leftover 255.99 marked stale");

    RadioCertification::Options o;
    o.settleMs = 0;
    RadioCertificationTestAccess::meterScale(cert, o);
    const QJsonObject stage = stageNamed(cert, QStringLiteral("meter-scale"));
    const QJsonObject m = stage.value(QStringLiteral("measured")).toObject();
    check(!stage.isEmpty(), "meter-scale stage was recorded");
    check(m.value(QStringLiteral("swr")).isNull(),
          "meter-scale stage: the leftover SWR is not in the report");
    check(!stage.value(QStringLiteral("concern")).toString().contains(QLatin1String("saturated")),
          "meter-scale stage: no saturation concern from a leftover");
    check(m.value(QStringLiteral("keyConfirmed")).toBool(true) == false,
          "meter-scale stage: the report says the key was not confirmed");
}

// Runs the three audio stages and returns them by id. The engine is never
// started, so an armed capture still receives no audio.
struct AudioStages {
    QJsonObject rxSidebands;
    QJsonObject sideband;
    QJsonObject lifecycle;
    int keyRefusals = -1;
};

// `feedAudio`: receive audio flows through the device-free Line Out path, so
// a capture fills; only lifecycle is run.
AudioStages runAudioStages(bool feedAudio = false)
{
    RadioModel radio;
    auto* backend = new IdleBackend();
    backend->linked = false;
    std::unique_ptr<IRadioBackend> owned(static_cast<IRadioBackend*>(backend));
    radio.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
    const bool fixture = radio.automationApplySliceFixture(0, QStringLiteral("A"));
    backend->linked = true;
    if (!fixture || !radio.slice(0)) {
        std::fprintf(stderr, "FATAL: the slice fixture was not installed\n");
        std::exit(2);
    }
    AudioEngine audio;
    PcmProducer producer;
    QTimer feeder;
    if (feedAudio) {
        audio.setLineOutCaptureEnabled(true);
        producer.start(PcmPurpose::Speaker, -1, {48000, PcmLayout::Stereo});
        QObject::connect(&feeder, &QTimer::timeout, &audio, [&]() {
            const std::optional<PcmFrame> frame =
                producer.produce(QVector<float>(2 * 480, 0.1f));
            if (frame)
                audio.feedPcmFrame(*frame);
        });
        feeder.start(10);
    }
    RadioCertification cert(&radio, &audio);
    RadioCertification::Options o;
    o.settleMs = 0;
    if (!feedAudio) {
        RadioCertificationTestAccess::rxSidebands(cert, o);
        RadioCertificationTestAccess::sideband(cert, o);
    }
    RadioCertificationTestAccess::lifecycle(cert, o);
    feeder.stop();
    AudioStages out;
    out.rxSidebands = stageNamed(cert, QStringLiteral("rx-sidebands"));
    out.sideband = stageNamed(cert, QStringLiteral("sideband"));
    out.lifecycle = stageNamed(cert, QStringLiteral("lifecycle"));
    out.keyRefusals = RadioCertificationTestAccess::keyRefusals(cert);
    return out;
}

QString concernOf(const QJsonObject& stage)
{
    return stage.value(QStringLiteral("concern")).toString();
}

void testCaptureRefused()
{
    // THE REGRESSION: the bridge running without AETHER_AUTOMATION=1.
    qunsetenv("AETHER_AUTOMATION");
    const AudioStages s = runAudioStages();
    const QString unavailable = QStringLiteral("audio capture unavailable");
    const QString engineReason = QStringLiteral("requires AETHER_AUTOMATION=1");

    check(!s.rxSidebands.isEmpty(), "refused: rx-sidebands was recorded");
    check(concernOf(s.rxSidebands).contains(unavailable)
              && concernOf(s.rxSidebands).contains(engineReason),
          "refused: rx-sidebands says the capture was unavailable, and why");
    check(!concernOf(s.rxSidebands).contains(QLatin1String("real carrier present")),
          "refused: rx-sidebands does not blame the antenna or the signal");
    const QJsonObject usb = s.rxSidebands.value(QStringLiteral("measured")).toObject()
                                .value(QStringLiteral("USB")).toObject();
    check(!usb.contains(QStringLiteral("toneAtOffsetDb")),
          "refused: rx-sidebands publishes no tone figure it never measured");

    check(concernOf(s.sideband).contains(unavailable),
          "refused: sideband says the capture was unavailable");
    check(!concernOf(s.sideband).contains(QLatin1String("TX audio monitor")),
          "refused: sideband does not blame the TX audio monitor");

    check(!s.lifecycle.isEmpty(), "refused: lifecycle was recorded");
    check(concernOf(s.lifecycle).contains(unavailable),
          "refused: lifecycle does not pass on a capture that never ran");

    check(s.keyRefusals == 0,
          "refused: no stage tried to key without a capture to measure it");
}

void testCaptureArmedButSilent()
{
    // The capture arms, the engine is not running, so nothing arrives: a
    // missing audio path, which is not the same report as a quiet band.
    qputenv("AETHER_AUTOMATION", "1");
    const AudioStages s = runAudioStages();
    qunsetenv("AETHER_AUTOMATION");

    const QString rx = concernOf(s.rxSidebands);
    check(rx.contains(QLatin1String("returned no audio"))
              && rx.contains(QLatin1String("USB")) && rx.contains(QLatin1String("DIGL")),
          "silent: rx-sidebands names the modes whose capture returned no audio");
    check(!rx.contains(QLatin1String("real carrier present")),
          "silent: rx-sidebands does not blame the antenna or the signal");
    const QJsonObject usb = s.rxSidebands.value(QStringLiteral("measured")).toObject()
                                .value(QStringLiteral("USB")).toObject();
    check(usb.value(QStringLiteral("frames")).toInt(-1) == 0
              && usb.value(QStringLiteral("toneAtOffsetDb")).isNull()
              && usb.value(QStringLiteral("overallRmsDb")).isNull(),
          "silent: an empty capture has frames 0 and no -240 dB levels");

    const QString pattern = s.rxSidebands.value(QStringLiteral("measured")).toObject()
                                .value(QStringLiteral("summary")).toObject()
                                .value(QStringLiteral("pattern")).toString();
    check(pattern == QLatin1String("no-audio"),
          "silent: rx-sidebands summary.pattern says no-audio, matching its concern");

    // No TxController, so the first leg's key is refused: unkeyed receive
    // audio is no sideband comparison, and the second leg is not keyed.
    const QString sb = concernOf(s.sideband);
    check(sb.contains(QLatin1String("INCONCLUSIVE"))
              && sb.contains(QLatin1String("key was refused")),
          "silent: sideband is INCONCLUSIVE when the key was refused");
    check(!sb.contains(QLatin1String("TX audio monitor"))
              && !sb.contains(QLatin1String("DISAGREE")),
          "silent: sideband blames neither the TX monitor nor the TX chain");
    check(s.sideband.value(QStringLiteral("measured")).toObject()
                  .value(QStringLiteral("keyConfirmed")).toBool(true) == false,
          "silent: sideband reports keyConfirmed false");
    check(s.keyRefusals == 1,
          "silent: sideband stops keying after the first refused key");
    const QJsonObject sbMeasured = s.sideband.value(QStringLiteral("measured")).toObject();
    check(!sbMeasured.contains(QStringLiteral("demodMatched"))
              && !sbMeasured.contains(QStringLiteral("demodOpposite"))
              && !sbMeasured.contains(QStringLiteral("matchedMinusOppositeDb")),
          "silent: a refused key publishes no leg levels, so no -240 dB figures");

    const QString life = concernOf(s.lifecycle);
    check(life.contains(QLatin1String("INCONCLUSIVE"))
              && life.contains(QLatin1String("no audio before")),
          "silent: lifecycle is INCONCLUSIVE when no audio flowed before the key");
}

void testLifecycleKeyRefusedWithAudio()
{
    // Audio flows before the key, so the stage reaches the key; with no
    // TxController it is refused, and no window may be reported as "during TX".
    qputenv("AETHER_AUTOMATION", "1");
    const AudioStages s = runAudioStages(true);
    qunsetenv("AETHER_AUTOMATION");

    const QJsonObject m = s.lifecycle.value(QStringLiteral("measured")).toObject();
    check(m.value(QStringLiteral("rxBytesBeforeKey")).toInt() > 0,
          "fixture: receive audio flowed before the key");
    check(s.keyRefusals == 1, "refused key: lifecycle tried to key once and was refused");
    const QString life = concernOf(s.lifecycle);
    check(life.contains(QLatin1String("INCONCLUSIVE"))
              && life.contains(QLatin1String("no window was taken during transmit")),
          "refused key: lifecycle is INCONCLUSIVE and takes no transmit window");
    check(!m.contains(QStringLiteral("rxBytesDuringTx"))
              && !m.contains(QStringLiteral("rxBytesAfterUnkey")),
          "refused key: lifecycle publishes no unkeyed window as during or after transmit");
    check(m.value(QStringLiteral("keyConfirmed")).toBool(true) == false,
          "refused key: lifecycle reports keyConfirmed false");
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radiocert-unmeasured-test"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "settings are isolated");

    testCaptureRefused();
    testCaptureArmedButSilent();
    testLifecycleKeyRefusedWithAudio();
    testMeterScaleJudgement();
    testMeterScaleStage();

    std::printf("\n%d checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
