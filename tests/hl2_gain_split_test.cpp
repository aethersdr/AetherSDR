// THE OPERATOR'S LNA NUMBER SURVIVES AN AUTOMATIC ONE.
//
// Two halves, because the property has two homes:
//
//   * the arithmetic, exhaustively — `effectiveLnaGain` never returns a gain
//     above the operator's baseline, never leaves the register's range, and
//     never reports having applied more attenuation than it did;
//   * the backend, behaviourally — applying an automatic offset moves the wire,
//     the dB reference and every pan's echo, and moves NOTHING that is
//     persisted: not `m_lnaDbByBand`, not the `rfGain` extension object that
//     `RadioStateMemory` writes to disk.
//
// WHY THE SECOND HALF IS A BAND ROUND TRIP AND NOT A GETTER CHECK. The loss
// this guards against is silent and delayed. Nothing looks wrong at the moment
// an automatic control acts; the stored entry dies later, on a band change,
// because `Hl2Backend::rememberCurrentBandState` records the live value under
// the band being left. A test that only read the value back after the offset
// was applied would pass against the very implementation that loses the data.
// So the sequence is: set a baseline, apply an offset, LEAVE THE BAND, come
// back, and only then ask what is on disk.
//
// This is the shape of an already-open defect in this lab's acceptance table
// (`D-lna-overwrite`: 40 m went -6 -> -12 dB on disk with no operator action
// beyond one band change), which is why an automatic writer must not be added
// to that path before the split exists.
//
// The session harness is `hl2_gain_restore_test`'s, deliberately: same
// socket-free connect, same "no event loop is pumped" constraint.

#include "SeamThreadAffinityProbe.h"
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/RadioStateMemory.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2GainSplit.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* label)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", label);
    if (!condition) {
        ++failures;
    }
}

int bandGain(const RestoredRadioState& state, const QString& band)
{
    return state.extension.value(QStringLiteral("rfGain")).toObject()
        .value(QStringLiteral("lnaDbByBand")).toObject().value(band).toInt(999);
}

RestoredRadioState rememberedGain()
{
    RestoredRadioState state;
    state.rfFrequencyHz = 14'074'000.0;
    state.sampleRateHz = 48'000;
    state.extensionSchemaVersion = 1;
    state.extension = QJsonObject{
        {QStringLiteral("rfGain"), QJsonObject{
            {QStringLiteral("defaultDb"), 20},
            {QStringLiteral("lnaDbByBand"), QJsonObject{
                {QStringLiteral("20m"), -6}, {QStringLiteral("40m"), -12}}}}}};
    return state;
}

// IRadioBackend contract rule 2 for autoRfGainArmSettled, observed rather
// than tabled: this binary is the only socket-free test that makes the
// signal fire, so every backend it drives carries the seam probe and the
// label names the offending emission when one crosses a thread.
void checkSeam(const test::SeamThreadAffinityProbe& seam, const char* where)
{
    const QStringList v = seam.violations();
    check(v.isEmpty(),
          qPrintable(QStringLiteral("%1: every seam signal arrived on the backend's thread (rule 2)%2")
                         .arg(QLatin1String(where),
                              v.isEmpty() ? QString() : QStringLiteral(" -- ") + v.join(QStringLiteral("; ")))));
}

// Exercise synchronous connect seeding without starting transport, exactly as
// hl2_gain_restore_test does: boardMaxRx skips the unicast discovery socket and
// no event loop is pumped, so finishDspSetup never runs.
class Session {
public:
    QString panId;
    int echoedGain = 999;
    hl2::Hl2Backend backend;
    test::SeamThreadAffinityProbe seam{&backend};   // after backend: torn down first

    explicit Session(const RestoredRadioState& state)
    {
        test::attachAllSeamSignals(seam);
        QObject::connect(&backend, &IRadioBackend::panCenterBandwidthChanged,
                         &backend, [this](const QString& id, double, double) {
            panId = id;
        });
        QObject::connect(&backend, &IRadioBackend::panRfGainChanged,
                         &backend, [this](const QString&, int gain) {
            echoedGain = gain;
        });
        backend.applyRestoredState(state);
        RadioConnectRequest request;
        request.host = QStringLiteral("192.0.2.1");
        request.serial = QStringLiteral("AA:BB:CC:DD:EE:02");
        request.params.insert(QStringLiteral("boardMaxRx"), 4);
        backend.connectRadio(request);
        backend.setSliceFrequency(0, state.rfFrequencyHz);
    }
    ~Session()
    {
        backend.disconnectRadio();
        checkSeam(seam, "session");
    }

    int healthLive() const
    {
        return backend.healthSnapshot().values
            .value(QStringLiteral("lnaGainDb"), 999).toInt();
    }
    int healthBaseline() const
    {
        return backend.healthSnapshot().values
            .value(QStringLiteral("lnaBaselineDb"), 999).toInt();
    }
    int healthOffset() const
    {
        return backend.healthSnapshot().values
            .value(QStringLiteral("lnaAutoOffsetDb"), 999).toInt();
    }
};

constexpr int kMin = -12;
constexpr int kMax = 48;

}  // namespace

int main(int argc, char** argv)
{
    using AetherSDR::hl2::effectiveLnaGain;

    // ---- 1. The arithmetic, EXHAUSTIVELY over the register's whole range ----
    //
    // Not a sampled property: the axis is 61 gains by 61 offsets, which is 3721
    // cases, so there is no reason to argue about coverage.
    {
        bool everAboveBaseline = false;
        bool everOutOfRange = false;
        bool everOverclaimedOffset = false;
        bool everNegativeApplied = false;
        bool floorFlagEverWrong = false;
        for (int baseline = kMin; baseline <= kMax; ++baseline) {
            for (int offset = 0; offset <= kMax - kMin; ++offset) {
                const auto e = effectiveLnaGain(baseline, offset, kMin, kMax);
                if (e.effectiveDb > baseline) everAboveBaseline = true;
                if (e.effectiveDb < kMin || e.effectiveDb > kMax) everOutOfRange = true;
                if (e.appliedOffsetDb > offset) everOverclaimedOffset = true;
                if (e.appliedOffsetDb < 0) everNegativeApplied = true;
                // floorReached must mean exactly "the request did not fit".
                if (e.floorReached != (baseline - offset < kMin)) floorFlagEverWrong = true;
            }
        }
        check(!everAboveBaseline,
              "no (baseline, offset) pair puts the wire ABOVE the operator's number");
        check(!everOutOfRange,
              "no pair leaves the AD9866's -12..+48 dB register range");
        check(!everNegativeApplied, "the applied offset is never negative");
        check(!everOverclaimedOffset,
              "the applied offset never EXCEEDS the requested one — a clamp under-"
              "delivers, and says so, rather than lying about what it took");
        check(!floorFlagEverWrong,
              "floorReached is set exactly when baseline - offset falls below the floor");
    }

    // ---- 2. A NEGATIVE offset is not an automatic gain increase -------------
    //
    // The axis is one-directional by construction. A caller passing -6 must not
    // get +6 dB of gain it never asked the operator for.
    {
        const auto e = effectiveLnaGain(0, -6, kMin, kMax);
        check(e.effectiveDb == 0 && e.appliedOffsetDb == 0,
              "a negative offset resolves to zero, never to a gain increase");
    }

    // ---- 3. The floor under-delivers and reports it ------------------------
    {
        const auto e = effectiveLnaGain(-6, 26, kMin, kMax);
        check(e.effectiveDb == kMin, "26 dB below -6 clamps at the register floor");
        check(e.appliedOffsetDb == 6, "and reports 6 dB applied, not the 26 requested");
        check(e.floorReached, "and flags that the controller has run out of range");
    }

    TestSettingsProfile profile(QStringLiteral("aether-hl2-gain-split"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    const RadioSettingsScope scope(QStringLiteral("hl2"),
                                   QStringLiteral("AA:BB:CC:DD:EE:02"));

    // ---- 4. The backend: an offset moves the wire and nothing on disk ------
    {
        Session s(rememberedGain());
        check(!s.panId.isEmpty(), "connect seeding creates a usable pan identity");
        check(s.healthLive() == -6 && s.healthOffset() == 0,
              "the session comes up on 20 m's stored -6 dB with no automatic offset");

        s.backend.setPanRfGain(s.panId, 5);
        check(s.backend.lnaBaselineDb() == 5 && s.healthLive() == 5,
              "the operator sets +5 dB and it reaches the wire");

        s.backend.setLnaAutoOffsetDb(9);
        check(s.backend.lnaEffectiveDb() == -4,
              "a 9 dB automatic offset puts -4 dB on the wire");
        check(s.echoedGain == -4,
              "and every pan is told -4, because hiding a gain change is its own defect");
        check(s.healthLive() == -4, "the health row reports what the radio is running");
        check(s.backend.lnaBaselineDb() == 5 && s.healthBaseline() == 5,
              "AND THE OPERATOR'S BASELINE IS STILL +5");
        check(s.healthOffset() == 9, "with the offset visible as its own number");

        // The delayed half. A band change is what persists, so this is where a
        // servo routed through applyLnaGainDb destroys the stored value.
        check(bandGain(s.backend.currentOperatingState(), QStringLiteral("20m")) == 5,
              "the persisted 20 m entry is the operator's +5, not the servoed -4");
        s.backend.setSliceFrequency(0, 7'074'000.0);
        check(bandGain(s.backend.currentOperatingState(), QStringLiteral("20m")) == 5,
              "LEAVING the band writes back +5, not -4 — D-lna-overwrite's shape");
        check(bandGain(s.backend.currentOperatingState(), QStringLiteral("40m")) == -12,
              "and 40 m still has its own stored -12");
        // The offset SURVIVES the band change and is applied below whatever the
        // NEW band's baseline is: it describes the antenna, not the band memory.
        // 40 m's stored baseline is -12, which is already the register floor, so
        // this also exercises the clamp arriving through a band change rather
        // than through a setter — and the baseline still reads as stored.
        check(s.backend.lnaBaselineDb() == -12,
              "on 40 m the baseline is that band's own stored -12");
        check(s.backend.lnaEffectiveDb() == -12 && s.backend.lnaAutoOffsetDb() == 9,
              "the offset is still held across the band change, clamped at the floor "
              "without moving the baseline");

        s.backend.setSliceFrequency(0, 14'074'000.0);
        check(s.backend.lnaBaselineDb() == 5,
              "returning to 20 m restores the operator's +5 as the baseline");
        check(s.backend.lnaEffectiveDb() == -4, "with the automatic offset still held");

        // ---- 5. Disarming restores the operator's number in ONE action -----
        s.backend.setLnaAutoOffsetDb(0);
        check(s.backend.lnaEffectiveDb() == 5 && s.echoedGain == 5,
              "clearing the offset puts the operator's +5 back on the wire at once");
        check(bandGain(s.backend.currentOperatingState(), QStringLiteral("20m")) == 5,
              "and the stored entry was +5 throughout");

        check(RadioStateMemory::store(scope, s.backend.capabilities(),
                                      s.backend.currentOperatingState()),
              "the snapshot stores through the production path");
    }

    // ---- 6. What reached DISK is the baseline, across a restart ------------
    //
    // The strongest form of the claim: not "the getter says 5" but "a new
    // session, reading what the previous one persisted, comes up on 5".
    {
        RadioCapabilities caps;
        {
            Session probe(rememberedGain());
            caps = probe.backend.capabilities();
        }
        Session s(RadioStateMemory::load(scope, caps));
        check(s.backend.lnaBaselineDb() == 5,
              "a fresh session restores +5 — the automatic -4 never reached disk");
        check(s.backend.lnaAutoOffsetDb() == 0,
              "and comes up with no automatic offset: it is session state, not memory");
    }

    // ---- 7. The offset cannot strand the receiver below the register ------
    {
        Session s(rememberedGain());
        s.backend.setPanRfGain(s.panId, -6);
        s.backend.setLnaAutoOffsetDb(26);
        check(s.backend.lnaEffectiveDb() == kMin,
              "26 dB below -6 dB stops at the register floor rather than wrapping");
        check(s.backend.lnaBaselineDb() == -6,
              "and the operator's -6 is untouched by the clamp");
        s.backend.setLnaAutoOffsetDb(-3);
        check(s.backend.lnaEffectiveDb() == -6,
              "a negative offset is refused at the backend too, resolving to zero");
    }

    // ---- 8. THE AUTOMATIC CONTROL'S SWITCH ---------------------------------
    //
    // The law itself is exercised as a pure function in
    // hl2_auto_gain_policy_test. What is tested HERE is the part that only
    // exists at the backend: arming, refusing to arm, and the guarantee that
    // switching it off restores the operator's number in one action.
    {
        Session s(rememberedGain());
        check(s.backend.autoRfGainControl() != nullptr,
              "the HL2 declares the automatic RF-gain capability");
        check(!s.backend.autoRfGainEnabled(),
              "and it is OFF on a fresh session, with no setting to say otherwise");

        // ---- THE REFUSAL, and whether the region it guards is REACHABLE.
        //
        // Above kAutoRfGainMaxBaselineDb this radio's gain axis is not
        // trustworthy: #5354 measured +48 dB reading identically to +18 dB, and
        // nothing in the gateware decode or the AD9866's stated geometry
        // accounts for it. The control declines and says why; it does NOT
        // quietly move the operator's number to somewhere it would work.
        //
        // WRITTEN AGAINST BOTH CONSTANTS RATHER THAN THE LITERAL 20, because
        // #5752 moves the axis ceiling to +19 -- the SAME hardware fact,
        // reached independently -- and once the ceiling equals the control's
        // own threshold, no baseline in the untrusted region can be set at all.
        // The case then flips from "refuses" to "there is nothing to refuse",
        // and a hand-typed 20 would report that as a failure. Found by building
        // the integration branch, where this PR and #5752 sit together; neither
        // branch can see it alone.
        const int untrusted = hl2::Hl2Backend::kAutoRfGainMaxBaselineDb + 1;
        s.backend.setPanRfGain(s.panId, untrusted);
        const bool reachable = s.backend.lnaBaselineDb() > 
                               hl2::Hl2Backend::kAutoRfGainMaxBaselineDb;
        s.backend.setAutoRfGain(true);
        if (reachable) {
            check(!s.backend.autoRfGainEnabled(),
                  "arming is REFUSED from a baseline in the fold region");
            check(s.backend.lnaBaselineDb() == untrusted,
                  "and the operator's number is not moved to make the feature work");
            check(s.healthLive() == untrusted,
                  "nor is the wire quietly attenuated in its place");
        } else {
            // The axis ceiling has been brought down onto the threshold, so the
            // setter clamped and the baseline is trustworthy by construction.
            check(s.backend.autoRfGainEnabled(),
                  "with the axis clamped AT the threshold, no untrusted baseline "
                  "is reachable and arming succeeds");
            check(s.backend.lnaBaselineDb()
                      == hl2::Hl2Backend::kAutoRfGainMaxBaselineDb,
                  "and the request came up at the ceiling rather than past it");
            s.backend.setAutoRfGain(false);
        }

        // At the boundary it arms: the limit is where the measurement puts it.
        s.backend.setPanRfGain(s.panId, 19);
        s.backend.setAutoRfGain(true);
        check(s.backend.autoRfGainEnabled(),
              "+19 dB is inside the trusted range and arming succeeds");
        check(s.backend.lnaAutoOffsetDb() == 0,
              "arming alone takes no gain — it acts on evidence, not on being armed");
        check(s.healthLive() == 19 && s.backend.lnaBaselineDb() == 19,
              "and neither the wire nor the baseline moved at the moment of arming");

        // ---- DISARMING RESTORES IN ONE ACTION, FROM ANY STATE. Simulate the
        // loop having taken gain, then switch it off. A control that left the
        // radio attenuated after being turned off would not undo itself.
        s.backend.setLnaAutoOffsetDb(11);
        check(s.backend.lnaEffectiveDb() == 8 && s.echoedGain == 8,
              "with 11 dB of offset held, the wire is 8 dB and every pan knows");
        s.backend.setAutoRfGain(false);
        check(!s.backend.autoRfGainEnabled(), "the switch reports itself off");
        check(s.backend.lnaAutoOffsetDb() == 0,
              "and the offset is surrendered");
        check(s.backend.lnaEffectiveDb() == 19 && s.echoedGain == 19,
              "the operator's +19 is back on the wire, in ONE action");
        check(bandGain(s.backend.currentOperatingState(), QStringLiteral("20m")) == 19,
              "and what is stored for the band is +19 throughout — never the 8");

        // ---- THE OPERATOR'S FLOOR. The second, and last, of the two numbers
        // they own. 26 dB is a CHOSEN default inside a MEASURED bound: from
        // the stock +20 dB baseline it reaches -6 dB, which is the first gain
        // #5354's own sweep measures as clean on this station.
        check(s.backend.autoRfGainFloorDb() == 26,
              "the floor defaults to 26 dB below the operator's setting");
        s.backend.setAutoRfGainFloorDb(9);
        check(s.backend.autoRfGainFloorDb() == 9, "and the operator can pull it in");
        s.backend.setAutoRfGainFloorDb(500);
        check(s.backend.autoRfGainFloorDb() == hl2::Hl2Backend::kAutoRfGainFloorMaxDb,
              "an absurd floor clamps rather than being taken literally");
        s.backend.setAutoRfGainFloorDb(-4);
        check(s.backend.autoRfGainFloorDb() == 0,
              "and a negative one resolves to zero — 'may take no gain at all'");
    }

    // ---- WHAT `autoEnabled` MEANS ON DISK: THE WISH, NOT THE RUNNING FLAG.
    //
    // The two are the same number until the backend DECLINES to arm, which it
    // does from any baseline above kAutoRfGainMaxBaselineDb because the gain
    // axis is not trustworthy there. At that moment "the operator wants this"
    // and "the loop is running" diverge, and only the first belongs on disk:
    // an explicit `false` is honoured forever, so persisting the running flag
    // would silently and permanently withdraw a preference the operator never
    // withdrew -- and would keep doing so after they lowered RF Gain into the
    // region where it would have worked.
    //
    // Nothing exercised this key before, which is why the suite was green
    // across a revision that had it backwards.
    {
        RestoredRadioState st;
        st.rfFrequencyHz = 14'200'000.0;
        Session s(st);

        // A baseline the loop refuses: above the trusted ceiling.
        s.backend.setPanRfGain(s.panId, hl2::Hl2Backend::kAutoRfGainMaxBaselineDb + 1);
        s.backend.setAutoRfGain(true);
        check(!s.backend.isArmed(),
              "the loop declines to arm from an untrusted baseline");

        const QJsonObject declined =
            s.backend.currentOperatingState().extension
                .value(QStringLiteral("rfGain")).toObject();
        check(declined.value(QStringLiteral("autoEnabled")).toBool() == true,
              "a DECLINED arm still persists the wish as true — the operator "
              "asked, the radio refused, and the asking is what survives");

        // Now a baseline it trusts: the same wish arms, and still reads true.
        s.backend.setPanRfGain(s.panId, hl2::Hl2Backend::kAutoRfGainMaxBaselineDb);
        s.backend.setAutoRfGain(true);
        check(s.backend.isArmed(), "and arms from a trusted one");
        const QJsonObject armed =
            s.backend.currentOperatingState().extension
                .value(QStringLiteral("rfGain")).toObject();
        check(armed.value(QStringLiteral("autoEnabled")).toBool() == true,
              "which persists as true too");

        // Switching it OFF is a real withdrawal and must persist as false.
        s.backend.setAutoRfGain(false);
        const QJsonObject off =
            s.backend.currentOperatingState().extension
                .value(QStringLiteral("rfGain")).toObject();
        check(off.value(QStringLiteral("autoEnabled")).toBool() == false,
              "and switching it off persists false — an explicit refusal is "
              "the one thing that must be honoured next launch");
    }

    // ---- ABSENT MEANS OFF. RFC #5535 asked for armed-by-default; the shipped
    // LNA default of +20 dB sits one dB above the baseline the loop will arm
    // from, so default-on would refuse on every fresh connect. Until the gain
    // axis is trustworthy at that default, a document with no key reads false.
    {
        RestoredRadioState st;
        st.rfFrequencyHz = 14'200'000.0;
        Session s(st);
        check(!s.backend.isArmed(),
              "a fresh profile does not arm the loop");
        check(hl2::kLnaDefaultGainDb > hl2::Hl2Backend::kAutoRfGainMaxBaselineDb,
              "and the reason is arithmetic: the shipped LNA default is above "
              "the baseline the loop will arm from, so default-on would only "
              "ever warn");
    }

    // ---- A DECLINED ARM MUST BE ABLE TO EXPLAIN ITSELF (#5817) ----
    //
    // Reading isArmed() back tells a caller THAT the request failed. Until now
    // the only account of WHY went to a qWarning, so the operator saw a
    // checkbox spring back to unticked in silence -- and on a fresh install
    // that is what the very first tick of Auto does, because no stored gain for
    // the band leaves the constructed baseline above the ceiling that gates
    // arming.
    {
        hl2::Hl2Backend fresh;
        check(fresh.lastArmRefusalReason().isEmpty(),
              "a backend that has not been asked to arm has no refusal to give");

        fresh.setAutoRfGain(true);
        const QString why = fresh.lastArmRefusalReason();
        check(!fresh.autoRfGainEnabled(),
              "arming from the constructed baseline is declined, as #5817 found");
        check(!why.isEmpty(),
              "and the refusal now carries a reason rather than only a log line");
        // The sentence has to name the two numbers the operator needs, or it is
        // not actionable: what their baseline is, and what it has to be below.
        check(why.contains(QString::number(AetherSDR::hl2::kLnaDefaultGainDb)),
              "the reason names the baseline that was refused");
        check(why.contains(QString::number(hl2::Hl2Backend::kAutoRfGainMaxBaselineDb)),
              "and the ceiling it has to be at or below");
        // AND NOTHING THE OPERATOR CANNOT USE. This sentence stopped being a
        // log line when it started being shown on the panadapter and read out
        // by a screen reader; an issue number is provenance for us and noise to
        // them. The citation stays on the qWarning, where it is still useful.
        check(!why.contains(QLatin1Char('#')),
              "and cites no issue number at the operator");

        // AND IT MUST NOT OUTLIVE THE REFUSAL IT DESCRIBES. Lower the baseline
        // under the ceiling, arm for real, and the reason has to go -- otherwise
        // a later unrelated failure would be shown this text.
        fresh.setPanRfGain(QString(), hl2::Hl2Backend::kAutoRfGainMaxBaselineDb - 1);
        fresh.setAutoRfGain(true);
        // ASSERTED, NOT SKIPPED. A guarded "[skip]" here would let a later
        // change that stops an unconnected backend from arming turn the
        // clear-on-success half green without ever running it.
        check(fresh.autoRfGainEnabled(),
              "lowering the baseline under the ceiling arms for real");
        check(fresh.lastArmRefusalReason().isEmpty(),
              "a successful arm clears the reason");

        // AND A RADIO SWAP TAKES IT WITH IT. applyRestoredState() is the reset
        // every new radio's document runs through; a reason composed about
        // radio A's baseline must not be what radio B's control reports before
        // it has been asked anything.
        hl2::Hl2Backend swapped;
        swapped.setAutoRfGain(true);
        check(!swapped.lastArmRefusalReason().isEmpty(),
              "precondition: the previous radio declined and said why");
        swapped.applyRestoredState(RestoredRadioState{});
        check(swapped.lastArmRefusalReason().isEmpty(),
              "a radio swap clears a reason composed about the previous radio");

        // AND EVERY OUTCOME IS ANNOUNCED. A view that only reads isArmed() back
        // after its own click never hears about the connect-time restore or a
        // bridge verb; autoRfGainArmSettled is what lets one handler cover all
        // three. Counted, so a no-op request is seen NOT to fire.
        hl2::Hl2Backend spoken;
        test::SeamThreadAffinityProbe seam(&spoken);   // rule 2, see checkSeam()
        test::attachAllSeamSignals(seam);
        int settled = 0;
        bool lastArmed = true;
        QObject::connect(&spoken, &IRadioBackend::autoRfGainArmSettled,
                         [&](bool armed) { ++settled; lastArmed = armed; });
        spoken.setAutoRfGain(true);
        check(settled == 1 && !lastArmed,
              "a refusal settles the request as not armed, so a view hears it");
        spoken.setPanRfGain(QString(), hl2::Hl2Backend::kAutoRfGainMaxBaselineDb - 1);
        spoken.setAutoRfGain(true);
        check(settled == 2 && lastArmed, "an arm settles as armed");
        spoken.setAutoRfGain(false);
        check(settled == 3 && !lastArmed, "a disarm settles as not armed");
        spoken.setAutoRfGain(false);
        check(settled == 3, "a request that changes nothing settles nothing");
        check(seam.count(QStringLiteral("autoRfGainArmSettled")) == 3,
              "the probe saw every emission the direct handler counted");
        checkSeam(seam, "spoken");
    }

    if (failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\nFAILURES PRESENT\n");
    return 1;
}
