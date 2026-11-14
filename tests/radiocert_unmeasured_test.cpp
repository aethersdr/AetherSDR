// radiocert must not report a reading it never took.
//
// #7084: meter-scale judged SWR without its stale flag, so a 255.99 left over
// from an earlier key raised a fixed-point saturation concern on a radio whose
// SWR meter was not fed at all.
//
// Socket-free: the stages run against a RadioModel with a backend installed for
// test and an AudioEngine that is constructed and never started. No
// TxController is given, so every key is refused and nothing can transmit.

#include "core/RadioCertification.h"
#include "core/AudioEngine.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "models/MeterModel.h"
#include "models/RadioModel.h"
#include "TestSettingsProfile.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <cstdio>
#include <memory>

namespace AetherSDR {

class RadioCertificationTestAccess {
public:
    static QJsonArray stages(const RadioCertification& cert) { return cert.m_stages; }
    static QJsonObject meterSnapshot(const RadioCertification& cert)
    {
        return cert.meterSnapshot();
    }
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

class IdleBackend final : public hl2::Hl2Backend {
public:
    bool isConnected() const override { return true; }
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

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radiocert-unmeasured-test"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "settings are isolated");

    testMeterScaleJudgement();
    testMeterScaleStage();

    std::printf("\n%d checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
