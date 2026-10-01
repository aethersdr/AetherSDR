// WHICH AUTOMATIC RF GAIN LAW A CONNECT INSTALLS.
//
// RFC #5535 approved the loop driving on the wideband bandscope with a floor of
// 24 dB, and the constructor installs that law. applyRestoredState() -- which
// RadioModel calls on every engaged connect, also with an empty state -- reset
// the law to the clip-counter "ramp" with a floor of 26. So a constructed
// backend and a connected one ran different laws, and no test read the law
// after a restore: hl2_auto_gain_policy_test pins the configurations
// themselves, not which one the backend installs.
//
// WHAT THIS PROVES, AND WHAT IT DOES NOT. Socket-free and radio-free. The link
// edges are fired through MetisClient's own signals, so the backend's real
// handlers run; telemetry and bandscope blocks arrive through the same signals
// the I/O thread emits. That proves which law is installed, that the backend
// ASKS for the bandscope gate when that law is armed on a connect, and that
// the law's decisions read the block that arrives. It does not prove a radio
// answers: MetisClient ignores the enable with no stream behind it, and the
// gate itself is hl2_ep4_gate_test's subject. The positive path on hardware is
// not certified here.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/hl2/Hl2AutoGainPolicy.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2BandscopeHeadroom.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QEvent>
#include <QJsonObject>
#include <cstdio>

namespace AetherSDR::hl2 {

struct Hl2AutoGainLawTestAccess {
    // The handlers under test are queued connections onto the backend's
    // thread, and this test never returns to an event loop to service them.
    static void drain(Hl2Backend& backend)
    {
        QCoreApplication::sendPostedEvents(&backend, QEvent::MetaCall);
    }
    static void linkUp(Hl2Backend& backend)
    {
        QMetaObject::invokeMethod(backend.m_metis, "linkUp",
                                  Qt::BlockingQueuedConnection);
        drain(backend);
    }
    static void linkDown(Hl2Backend& backend)
    {
        QMetaObject::invokeMethod(backend.m_metis, "linkDown",
                                  Qt::BlockingQueuedConnection);
        drain(backend);
    }
    // WHAT connectRadio() DOES WITH THE BAND'S REMEMBERED GAIN, done by hand.
    // connectRadio() itself hands the DSP opens to the I/O thread and its
    // completion would start the wire, which is what socket-free rules out.
    static void seedConnectBaseline(Hl2Backend& backend, int gainDb)
    {
        backend.m_lnaGainDb = gainDb;
    }
    static void telemetry(Hl2Backend& backend, const Hl2Telemetry& t)
    {
        MetisClient* metis = backend.m_metis;
        QMetaObject::invokeMethod(metis, [metis, t] {
            emit metis->telemetryUpdated(t);
        }, Qt::BlockingQueuedConnection);
        drain(backend);
    }
    static void deliverBlock(Hl2Backend& backend, const Ep4Stats& block)
    {
        MetisClient* metis = backend.m_metis;
        QMetaObject::invokeMethod(metis, [metis, block] {
            emit metis->bandscopeBlockReady(block);
        }, Qt::BlockingQueuedConnection);
        drain(backend);
    }
    static const AutoGainConfig& config(const Hl2Backend& backend)
    {
        return backend.m_autoGainConfig;
    }
    static AutoGainReason reason(const Hl2Backend& backend)
    {
        return backend.m_autoGainReason;
    }
    // TRUE when the backend has asked MetisClient for the bandscope gate on
    // the loop's behalf. The request, not the stream: see the file header.
    static bool gateAskedFor(const Hl2Backend& backend)
    {
        return backend.m_bandscopeOwnedByAutoGain;
    }
};

}  // namespace AetherSDR::hl2

using namespace AetherSDR;
using Access = AetherSDR::hl2::Hl2AutoGainLawTestAccess;

namespace {
int failures = 0;
void check(bool condition, const char* label)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", label);
    if (!condition) {
        ++failures;
    }
}

// THE RULING'S OWN NUMBERS, written here and not read from the code under
// test: #5535 point 3 approved the bandscope as the input, point 2 holds
// maxOffsetDb at 24 dB.
const QString kApprovedLaw = QStringLiteral("bandscope");
constexpr int kApprovedFloorDb = 24;
// Inside the range the loop arms from on any ceiling this branch can meet.
constexpr int kTrustedBaselineDb = 10;

QVariant row(const hl2::Hl2Backend& backend, const char* key)
{
    return backend.healthSnapshot().values.value(QString::fromLatin1(key));
}

int autoOffsetDb(const hl2::Hl2Backend& backend)
{
    return row(backend, "lnaAutoOffsetDb").toInt();
}

// A profile as currentOperatingState() writes it: the operator's switch and
// one band's gain. Those are the only automatic-gain facts that are persisted.
RestoredRadioState savedProfile(bool autoWanted)
{
    RestoredRadioState state;
    state.rfFrequencyHz = 14'074'000.0;
    state.sampleRateHz = 48'000;
    state.extensionSchemaVersion = 1;
    QJsonObject rfGain{
        {QStringLiteral("defaultDb"), 20},
        {QStringLiteral("lnaDbByBand"),
         QJsonObject{{QStringLiteral("20m"), kTrustedBaselineDb}}}};
    if (autoWanted) {
        rfGain.insert(QStringLiteral("autoEnabled"), true);
    }
    state.extension = QJsonObject{{QStringLiteral("rfGain"), rfGain}};
    return state;
}

QJsonObject storedRfGain(const hl2::Hl2Backend& backend)
{
    return backend.currentOperatingState().extension
        .value(QStringLiteral("rfGain")).toObject();
}

// One telemetry window of converter observations.
hl2::Hl2Telemetry window(int overloadSamples, int windowMs)
{
    hl2::Hl2Telemetry t;
    t.adcSamples = 100;
    t.adcOverloadSamples = overloadSamples;
    t.adcWindowMs = windowMs;
    return t;
}

// A bandscope block far from the rail: peak code 8 of 2048 is 48 dB of
// headroom, more than any release step plus its bias and margin.
hl2::Ep4Stats quietBlock()
{
    hl2::Ep4Stats block;
    block.samples = 2048;
    block.peakAbs = 8;
    block.sumSquares = 2048.0 * 16.0;
    return block;
}

// applyRestoredState, the hand-seeded connect baseline, then the link edge.
void connectWith(hl2::Hl2Backend& backend, const RestoredRadioState& state)
{
    backend.applyRestoredState(state);
    Access::seedConnectBaseline(backend, kTrustedBaselineDb);
    Access::linkUp(backend);
}
}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-hl2-auto-gain-law"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    // ---- 1. A CONSTRUCTED BACKEND RUNS THE APPROVED LAW --------------------
    // The reference every later section compares against, so it is pinned to
    // the ruling first: two backends agreeing with each other proves nothing
    // if both are wrong.
    {
        hl2::Hl2Backend fresh;
        check(fresh.law() == kApprovedLaw,
              "a constructed backend names the bandscope law");
        check(fresh.laws().value(0) == fresh.law(),
              "and it is the first law listed, which is the documented default");
        check(fresh.floorDb() == kApprovedFloorDb,
              "its floor is the 24 dB the ruling holds");
        check(Access::config(fresh).requireHeadroomToRelease,
              "and its release needs a measured headroom reading");
        check(row(fresh, "autoRfGainMode").toString() == kApprovedLaw,
              "the health row reports the same law");
        check(!fresh.isArmed(), "installing a law does not arm the loop");
    }

    // ---- 2. applyRestoredState({}) INSTALLS WHAT THE CONSTRUCTOR INSTALLS --
    // "This radio has no memory", which is what RadioModel hands over on a
    // first connect. Fails on main: law "ramp", floor 26.
    {
        hl2::Hl2Backend fresh;
        hl2::Hl2Backend restored;
        restored.applyRestoredState(RestoredRadioState{});
        check(restored.law() == fresh.law(),
              "after applyRestoredState({}) the law is the constructed one");
        check(restored.floorDb() == fresh.floorDb(),
              "after applyRestoredState({}) the floor is the constructed one");
        check(Access::config(restored) == Access::config(fresh),
              "after applyRestoredState({}) every field of the config matches");
        check(row(restored, "autoRfGainMode") == row(fresh, "autoRfGainMode")
                  && row(restored, "autoRfGainFloorDb") == row(fresh, "autoRfGainFloorDb")
                  && row(restored, "autoRfGainReleaseNeedsDb")
                         == row(fresh, "autoRfGainReleaseNeedsDb"),
              "and the health rows an operator reads agree with it");
        check(row(restored, "autoRfGainReleaseNeedsDb").isValid(),
              "positive control: the release-needs row exists, so the "
              "comparison above is not two absent values agreeing");
    }

    // ---- 3. A RESTORE WITH SAVED STATE: PERSISTED MEMBERS COME BACK, THE ----
    // ---- REST RETURN TO THE CONSTRUCTED DEFAULT ----------------------------
    // The law and the floor are session state. A session that selected
    // another law and a deeper floor must not leak them into the next
    // connect, and the operator's saved switch and gain must survive.
    {
        hl2::Hl2Backend fresh;
        hl2::Hl2Backend backend;
        check(backend.setLaw(QStringLiteral("ramp")) && backend.law() == QLatin1String("ramp"),
              "positive control: the session selected another law");
        backend.setFloorDb(hl2::Hl2Backend::kAutoRfGainFloorMaxDb);
        check(backend.floorDb() == hl2::Hl2Backend::kAutoRfGainFloorMaxDb,
              "positive control: and a deeper floor");

        backend.applyRestoredState(savedProfile(true));
        check(backend.law() == fresh.law(),
              "a restore with saved state installs the constructed law");
        check(Access::config(backend) == Access::config(fresh),
              "and the constructed config, floor included");
        const QJsonObject stored = storedRfGain(backend);
        check(stored.value(QStringLiteral("autoEnabled")).toBool(false),
              "the operator's saved switch is restored");
        check(stored.value(QStringLiteral("lnaDbByBand")).toObject()
                  .value(QStringLiteral("20m")).toInt(999) == kTrustedBaselineDb,
              "and so is the saved per-band gain");
        check(!backend.isArmed(),
              "the wish alone does not run the loop before the link is up");
    }

    // ---- 4. "default" NAMES THE DEFAULT ------------------------------------
    {
        hl2::Hl2Backend fresh;
        hl2::Hl2Backend backend;
        check(backend.setLaw(QStringLiteral("ramp")), "positive control: leave the default");
        check(backend.setLaw(QStringLiteral("default")), "\"default\" is accepted");
        check(backend.law() == fresh.law()
                  && Access::config(backend) == Access::config(fresh),
              "and installs the law a constructed backend has");
    }

    // ---- 5. AFTER A CONNECT, THE BANDSCOPE LAW IS WHAT STEPS ---------------
    {
        hl2::Hl2Backend backend;
        connectWith(backend, savedProfile(true));
        check(backend.isConnected(), "positive control: the link edge was delivered");
        check(backend.isArmed(),
              "a saved switch arms the loop at the connect edge");
        check(backend.law() == kApprovedLaw
                  && row(backend, "autoRfGainMode").toString() == kApprovedLaw,
              "the armed loop runs the bandscope law");
        check(Access::gateAskedFor(backend),
              "and the backend asked for the bandscope gate that law reads");

        // Clean windows first: nothing to do, nothing held.
        for (int i = 0; i < 3; ++i) {
            Access::telemetry(backend, window(0, 100));
        }
        check(autoOffsetDb(backend) == 0, "clean windows hold no offset");

        // An occasional clip. The ramp's first step out of a quiet period is
        // 3 dB for this rate; the bandscope law steps one 6 dB quantum.
        Access::telemetry(backend, window(10, 100));
        check(autoOffsetDb(backend) == 6,
              "an occasional clip is answered with the bandscope law's 6 dB "
              "step, not the ramp's 3");

        // Clean for longer than any dwell, with no bandscope block. The ramp
        // and the probing law would both have given gain back by now.
        for (int i = 0; i < 40; ++i) {
            Access::telemetry(backend, window(0, 1000));
        }
        check(autoOffsetDb(backend) == 6,
              "with no bandscope reading the law gives nothing back");
        check(Access::reason(backend) == hl2::AutoGainReason::HeadroomAbsent,
              "and says the reading is what it is waiting for");

        // The reading arrives and has room for the step.
        const double needsDb = row(backend, "autoRfGainReleaseNeedsDb").toDouble();
        const hl2::HeadroomObservation seen = hl2::bandscopeHeadroom(quietBlock(), 0);
        check(seen.isMeasurement() && needsDb > 0.0 && seen.headroomDb >= needsDb,
              "positive control: the block carries more headroom than a release needs");
        Access::deliverBlock(backend, quietBlock());
        Access::telemetry(backend, window(0, 1000));
        check(autoOffsetDb(backend) == 0,
              "a measured reading with room licenses the release");
        check(Access::reason(backend) == hl2::AutoGainReason::Release,
              "and the decision is recorded as a release");

        // ---- the link drops and resumes on the same session ----
        // MetisClient re-emits linkUp when EP6 resumes after a silence; no
        // connectRadio() and no applyRestoredState() run in between.
        Access::linkDown(backend);
        check(!Access::gateAskedFor(backend) && backend.isArmed(),
              "a link drop ends the claim on the gate and leaves the loop armed");
        Access::linkUp(backend);
        check(Access::gateAskedFor(backend),
              "the resumed link asks for the gate again");

        // ---- and a law that does not read the bandscope lets it go ----
        check(backend.setLaw(QStringLiteral("ramp")), "select the ramp");
        check(!Access::gateAskedFor(backend),
              "the ramp does not read the bandscope, so the gate is released");
        backend.disconnectRadio();
    }

    // ---- 6. OFF BY DEFAULT STILL MEANS NO STREAM ---------------------------
    // Installing the bandscope law on connect must not start the gate for an
    // operator who never switched the loop on.
    {
        hl2::Hl2Backend backend;
        connectWith(backend, savedProfile(false));
        check(backend.isConnected(), "positive control: the link edge was delivered");
        check(!backend.isArmed(), "a profile with no switch does not arm");
        check(backend.law() == kApprovedLaw, "the law is installed all the same");
        check(!Access::gateAskedFor(backend),
              "and no bandscope gate is asked for while the loop is off");
        backend.disconnectRadio();
    }

    return failures ? 1 : 0;
}
