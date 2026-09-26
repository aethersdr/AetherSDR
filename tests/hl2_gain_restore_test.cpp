#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/RadioStateMemory.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/anan/AnanBackend.h"
#include "gui/RfGainRestore.h"

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

// THE OPERATOR'S AUTOMATIC-GAIN PREFERENCE, as it will be written to disk.
// `m_autoRfGainWanted` is private and correctly has no accessor -- what it
// means is only observable where it acts, which is the document
// currentOperatingState() produces and applyRestoredState() reads back. Absent
// reads as false, matching applyRestoredState's own `toBool(false)`.
bool autoGainWanted(const RestoredRadioState& state)
{
    return state.extension.value(QStringLiteral("rfGain")).toObject()
        .value(QStringLiteral("autoEnabled")).toBool(false);
}

// A profile with no per-band gains, so the connect baseline comes from
// `defaultDb` and each leg below can set the baseline it needs explicitly.
RestoredRadioState autoGainProfile(bool wanted)
{
    RestoredRadioState state;
    state.rfFrequencyHz = 14'074'000.0;
    state.sampleRateHz = 48'000;
    state.extensionSchemaVersion = 1;
    QJsonObject rfGain{{QStringLiteral("defaultDb"), 20}};
    if (wanted) {
        rfGain.insert(QStringLiteral("autoEnabled"), true);
    }
    state.extension = QJsonObject{{QStringLiteral("rfGain"), rfGain}};
    return state;
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
                {QStringLiteral("20m"), -12}, {QStringLiteral("40m"), -6}}}}}};
    return state;
}

// WHAT THE APPLICATION ACTUALLY PERSISTS, reached the way it actually reaches it.
//
// RadioModel never polls currentOperatingState(). It connects
// IRadioBackend::operatingStateChanged to scheduleOperatingStateSave() and fetches
// the document INSIDE that handler, then hands the snapshot to
// RadioStateMemory::store. So a backend that moves persisted state without emitting
// leaves the flag correct in memory and the profile on disk carrying the old value.
//
// A test that calls currentOperatingState() directly cannot see that difference: it
// is performing the one read the application never performs, and it answers from the
// live flag every time. This mirror only ever samples the document when the backend
// says the document moved, which is the whole of the contract in IRadioBackend.
class ProfileMirror {
public:
    explicit ProfileMirror(hl2::Hl2Backend& backend)
    {
        m_conn = QObject::connect(&backend, &IRadioBackend::operatingStateChanged,
                                  &backend, [this, &backend] {
            m_stored = backend.currentOperatingState();
            ++m_saves;
        });
    }
    // Disconnected explicitly: the lambda captures this mirror, and ~GainSession
    // calls disconnectRadio() on a backend that outlives it.
    ~ProfileMirror() { QObject::disconnect(m_conn); }
    ProfileMirror(const ProfileMirror&) = delete;
    ProfileMirror& operator=(const ProfileMirror&) = delete;

    bool storedAutoGain() const { return autoGainWanted(m_stored); }
    int saves() const { return m_saves; }

private:
    QMetaObject::Connection m_conn;
    RestoredRadioState m_stored;
    int m_saves = 0;
};

// EVERY SETTLED VERDICT, AND WHAT A HANDLER WOULD HAVE SEEN WHEN IT ARRIVED.
//
// IRadioBackend::autoRfGainArmSettled is documented as emitted after EVERY
// outcome of setArmed() -- refused, armed and disarmed -- because a view that
// only reads isArmed() back after its own click never learns about an arm that
// settled somewhere else. A path that moves the control and emits nothing is
// invisible to MainWindow::onAutoRfGainArmSettled, and counting is the only way
// to see that: the flag it would have read is right either way.
//
// AND THE REASON IS SAMPLED INSIDE THE HANDLER, not afterwards, because the
// ORDER is a requirement and not a detail. onAutoRfGainArmSettled reads
// lastArmRefusalReason() on entry whenever `armed` is false and re-shows the
// refusal from it. A disarm that emitted before clearing the reason would put
// the old "declined" sentence in front of an operator who had just switched the
// control off -- green on any assertion taken after the call returns.
class SettledLog {
public:
    explicit SettledLog(hl2::Hl2Backend& backend)
    {
        m_conn = QObject::connect(&backend, &IRadioBackend::autoRfGainArmSettled,
                                  &backend, [this, &backend](bool armed) {
            ++m_settles;
            m_lastArmed = armed;
            m_reasonAtEmit = backend.lastArmRefusalReason();
        });
    }
    ~SettledLog() { QObject::disconnect(m_conn); }
    SettledLog(const SettledLog&) = delete;
    SettledLog& operator=(const SettledLog&) = delete;

    int settles() const { return m_settles; }
    bool lastArmed() const { return m_lastArmed; }
    QString reasonAtEmit() const { return m_reasonAtEmit; }

private:
    QMetaObject::Connection m_conn;
    int m_settles = 0;
    bool m_lastArmed = false;
    QString m_reasonAtEmit;
};

// Exercise synchronous connect seeding and capture without starting transport.
// boardMaxRx skips the unicast discovery socket. No event loop is pumped:
// finishDspSetup cannot run, and disconnect cancels it before destruction.
// TEST-NET-1 alone would NOT make this socket-free.
class GainSession {
public:
    QString panId;
    int echoedGain = 999;
    hl2::Hl2Backend backend;

    GainSession(const RestoredRadioState& state, std::optional<int> pin = std::nullopt)
    {
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
        request.serial = QStringLiteral("AA:BB:CC:DD:EE:01");
        request.params.insert(QStringLiteral("boardMaxRx"), 4);
        if (pin.has_value()) {
            request.params.insert(QStringLiteral("lnaGainDb"), *pin);
        }
        backend.connectRadio(request);
        backend.setSliceFrequency(0, state.rfFrequencyHz); // publish the pan identity
        check(!panId.isEmpty(), "connect seeding creates a usable pan identity");
    }
    ~GainSession() { backend.disconnectRadio(); }

    int liveGain() const
    {
        return backend.healthSnapshot().values.value(QStringLiteral("lnaGainDb"), 999).toInt();
    }
    int restoreDisplay(std::optional<int> savedGain, int& writes)
    {
        const RadioCapabilities caps = backend.capabilities();
        return restoreLegacyRfGain(caps.family,
            caps.clientSettingsDomains.testFlag(RadioCapabilities::ClientSettingsDomain::RfGain),
            savedGain, liveGain(), [this, &writes](int gain) {
                ++writes;
                backend.setPanRfGain(panId, gain);
            });
    }
};
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-hl2-gain-restore"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    const RadioSettingsScope scope(QStringLiteral("hl2"), QStringLiteral("AA:BB:CC:DD:EE:01"));
    RadioCapabilities caps;
    {
        GainSession session(rememberedGain());
        caps = session.backend.capabilities();
        check(caps.clientSettingsDomains.testFlag(RadioCapabilities::ClientSettingsDomain::RfGain),
              "HL2 retains the RF-gain domain required by per-band storage");
        int writes = 0;
        check(session.restoreDisplay(20, writes) == -12 && writes == 0,
              "startup displays the restored band gain without replaying the legacy +20");
        check(session.liveGain() == -12, "legacy display restore leaves live 20m gain at -12");
        session.backend.setSliceFrequency(0, 14'080'000.0);
        check(bandGain(session.backend.currentOperatingState(), QStringLiteral("20m")) == -12,
              "same-band capture preserves the saved 20m gain");
        session.backend.setSliceFrequency(0, 7'074'000.0);
        check(session.liveGain() == -6 && session.echoedGain == -6,
              "band hop applies and publishes 40m gain");
        session.backend.setSliceFrequency(0, 14'074'000.0);
        check(session.liveGain() == -12 && session.echoedGain == -12,
              "return to 20m applies and publishes its own gain");
        session.backend.setPanRfGain(session.panId, 5);
        check(session.liveGain() == 5 && session.echoedGain == 5,
              "operator gain change still applies and publishes");
        check(RadioStateMemory::store(scope, caps, session.backend.currentOperatingState()),
              "updated gain persists through the production OperatingState store");
    }
    {
        const RestoredRadioState reloaded = RadioStateMemory::load(scope, caps);
        // THE HEAL, PINNED WHERE THE CLAIM IS MADE (#5869 review, ten9876).
        // The absence checks elsewhere in this file read currentOperatingState(),
        // the IN-MEMORY capture; the sentence in Hl2Backend.cpp's
        // currentOperatingState() is about what lands in the DOCUMENT. This
        // state got there the production way: the session above was restored
        // from rememberedGain(), which seeds `defaultDb: 20`, and its capture
        // went through a real RadioStateMemory::store; this is the matching
        // load. So a stale key is shown to decay out of the stored document,
        // not merely out of the snapshot.
        //
        // It is also the standing guard against a merging store. store()
        // rebuilds the gated extension wholesale from state.extension today,
        // which is why dropping the key from the capture is enough -- if that
        // ever became a preserve-unknown-siblings merge, as
        // storeRtlRfGainPreservingLegacy is for its own family, the key would
        // survive on disk forever and this is the check that would say so.
        check(!reloaded.extension.value(QStringLiteral("rfGain")).toObject()
                   .contains(QStringLiteral("defaultDb")),
              "the stored document itself carries no LNA default after a real "
              "RadioStateMemory store/load round trip");
        GainSession session(reloaded);
        int writes = 0;
        check(session.restoreDisplay(20, writes) == 5 && writes == 0 && session.liveGain() == 5,
              "a recreated session restores the operator's +5 despite stale global +20");
        check(bandGain(session.backend.currentOperatingState(), QStringLiteral("40m")) == -6,
              "saving 20m leaves 40m unchanged");
    }
    {
        GainSession session(rememberedGain(), 20);
        check(session.liveGain() == 20, "explicit connect override really sets live gain to +20");
        check(bandGain(session.backend.currentOperatingState(), QStringLiteral("20m")) == -12,
              "production capture preserves -12 while the connect override is active");
        session.backend.setSliceFrequency(0, 14'080'000.0);
        check(bandGain(session.backend.currentOperatingState(), QStringLiteral("20m")) == -12,
              "same-band tune cannot persist the temporary override");
        session.backend.setSliceFrequency(0, 7'074'000.0);
        session.backend.setSliceFrequency(0, 14'074'000.0);
        check(session.liveGain() == -12, "band writeback preserves the overridden start band");
        session.backend.setPanRfGain(session.panId, 5);
        check(bandGain(session.backend.currentOperatingState(), QStringLiteral("20m")) == 5,
              "operator changes still reach the production snapshot after a pin");
    }
    // The same write, but of the PINNED VALUE ITSELF, on the start band. A
    // write that does not MOVE the gain is still the operator choosing that
    // value for this band, so it has to end the pin and record the band exactly
    // as a moving write does. setPanRfGain's equality early return used to sit
    // above both, so this operator got neither. (#5402 review nit 3.)
    {
        GainSession session(rememberedGain(), 20);
        check(session.liveGain() == 20 && bandGain(session.backend.currentOperatingState(),
                                                   QStringLiteral("20m")) == -12,
              "same-value case starts pinned at +20 with 20m still stored as -12");
        session.backend.setPanRfGain(session.panId, 20);
        check(bandGain(session.backend.currentOperatingState(), QStringLiteral("20m")) == 20,
              "an operator write of the pinned value itself records the band");
        session.backend.setSliceFrequency(0, 7'074'000.0);
        session.backend.setSliceFrequency(0, 14'074'000.0);
        check(session.liveGain() == 20,
              "the confirmed value survives a band round trip instead of reverting to -12");
    }
    // Existing stored gains above +19 retain their meaning and survive capture.
    for (const int gain : {20, 24, 32, 48}) {
        RestoredRadioState state = rememberedGain();
        QJsonObject rfGain = state.extension.value(QStringLiteral("rfGain")).toObject();
        rfGain.insert(QStringLiteral("defaultDb"), gain);
        QJsonObject bands = rfGain.value(QStringLiteral("lnaDbByBand")).toObject();
        bands.insert(QStringLiteral("20m"), gain);
        rfGain.insert(QStringLiteral("lnaDbByBand"), bands);
        state.extension.insert(QStringLiteral("rfGain"), rfGain);
        GainSession session(state);
        const RestoredRadioState captured = session.backend.currentOperatingState();
        check(session.liveGain() == gain,
              "stored native gain seeds the live snapshot without folding");
        // INVERTED BY #5829 in its second half only. The per-band value is
        // still preserved verbatim -- that is real operator memory and nothing
        // about it changed. The `defaultDb == gain` half asserted a round trip
        // of a key nothing could write; it is now asserted absent instead,
        // which is the behaviour that replaces the round trip.
        check(bandGain(captured, QStringLiteral("20m")) == gain
                  && !captured.extension.value(QStringLiteral("rfGain")).toObject()
                          .contains(QStringLiteral("defaultDb")),
              "capture preserves the stored band gain and emits no LNA default");
    }
    {
        GainSession session(rememberedGain(), 999);
        check(session.liveGain() == 48,
              "out-of-range connect gain seeds the same ceiling as the wire");
        check(bandGain(session.backend.currentOperatingState(), QStringLiteral("20m")) == -12,
              "clamped connect override still preserves stored gain while pinned");
    }
    // ---- #5829: a STALE rfGain.defaultDb must not steer the radio ---------
    //
    // The fix is worthless if an old profile keeps deciding the gain, so both
    // halves are asserted together: a profile with NO defaultDb and a profile
    // carrying a stale one must produce the SAME gain on a band neither has an
    // entry for, and that gain must be the shipped constant.
    //
    // -6 is not an arbitrary stale value. It is the number the reporting
    // station's profile actually held, frozen, with ten of its twelve stored
    // bands sitting at it because each inherited it on its first visit.
    //
    // 15m (21.074 MHz) is the unvisited band: rememberedGain() stores 20m and
    // 40m only, so the fallback is what decides, which is the whole subject.
    {
        RestoredRadioState absent = rememberedGain();
        QJsonObject withoutKey =
            absent.extension.value(QStringLiteral("rfGain")).toObject();
        withoutKey.remove(QStringLiteral("defaultDb"));
        absent.extension.insert(QStringLiteral("rfGain"), withoutKey);

        RestoredRadioState stuck = absent;
        QJsonObject withStaleKey = withoutKey;
        withStaleKey.insert(QStringLiteral("defaultDb"), -6);
        stuck.extension.insert(QStringLiteral("rfGain"), withStaleKey);

        int gainWithNoKey = 999;
        int gainWithStaleKey = 999;
        {
            GainSession session(absent);
            session.backend.setSliceFrequency(0, 21'074'000.0);   // unvisited 15m
            gainWithNoKey = session.liveGain();
        }
        {
            GainSession session(stuck);
            session.backend.setSliceFrequency(0, 21'074'000.0);   // unvisited 15m
            gainWithStaleKey = session.liveGain();

            // And the key does not survive a capture, which is what heals an
            // existing document: the next snapshot simply writes it away.
            check(!session.backend.currentOperatingState()
                       .extension.value(QStringLiteral("rfGain")).toObject()
                       .contains(QStringLiteral("defaultDb")),
                  "a restored stale defaultDb is dropped from the next capture");
        }
        check(gainWithNoKey == gainWithStaleKey,
              "a profile carrying a stale rfGain.defaultDb comes up on the same "
              "unvisited-band gain as a profile without the key");
        check(gainWithNoKey == hl2::kLnaDefaultGainDb,
              "and that gain is the shipped kLnaDefaultGainDb, which is now the "
              "only answer to what an unvisited band comes up on");
    }

    // No transport: apply the per-radio snapshot, then exercise the same
    // legacy-display helper called by MainWindow when the pan appears.
    {
        anan::AnanBackend backend;
        RestoredRadioState state;
        state.extensionSchemaVersion = 1;
        state.extension = QJsonObject{{QStringLiteral("rfGain"), QJsonObject{
            {QStringLiteral("adc0AttenuationDb"), 12},
            {QStringLiteral("adc1AttenuationDb"), 23}}}};
        backend.applyRestoredState(state);
        const RadioCapabilities ananCaps = backend.capabilities();
        int writes = 0;
        const int shown = restoreLegacyRfGain(ananCaps.family,
            ananCaps.clientSettingsDomains.testFlag(RadioCapabilities::ClientSettingsDomain::RfGain),
            -5, -backend.attenuationDbForTest(), [&backend, &writes](int gain) {
                ++writes;
                backend.setPanRfGain(QStringLiteral("0"), gain);
            });
        check(shown == -12 && writes == 0 && backend.attenuationDbForTest() == 12,
              "ANAN startup preserves per-radio attenuation despite stale family display gain");
        check(backend.currentOperatingState().extension == state.extension,
              "legacy display restore leaves both ANAN ADC values intact");
    }
    // A REFUSED ARM MUST NOT SWALLOW THE OPERATOR'S LATER "OFF" (#5828).
    //
    // setAutoRfGain's opening guard compares against the RUNNING flag, and a
    // declined arm leaves the loop off with the wish recorded -- so an explicit
    // "off" from there used to match the guard and return before the disarm
    // branch that clears the wish. The profile kept `autoEnabled: true` and the
    // next connect from a baseline the loop trusts armed a control the operator
    // had switched off.
    //
    // THE SHIPPED DEFAULT IS WHAT MAKES THIS THE COMMON PATH: the constructed
    // LNA default is kLnaDefaultGainDb (+20 dB) and kAutoRfGainMaxBaselineDb is
    // +19, so the first tick on a radio nobody has retuned lands in the refusal.
    //
    // Asserted on the persisted document rather than on a flag, because the
    // harm is not the flag -- it is the next session, which leg three shows.
    {
        constexpr int kCeiling = hl2::Hl2Backend::kAutoRfGainMaxBaselineDb;
        constexpr int kUntrusted = kCeiling + 1;  // +20 dB: the shipped default
        constexpr int kTrusted = kCeiling;        // +19 dB: the highest it takes
        {
            GainSession session(autoGainProfile(false));
            SettledLog settled(session.backend);
            session.backend.setPanRfGain(session.panId, kUntrusted);
            check(!autoGainWanted(session.backend.currentOperatingState()),
                  "nothing is wanted before the operator asks");
            session.backend.setAutoRfGain(true);
            check(!session.backend.autoRfGainEnabled(),
                  "the radio declines to arm from the shipped default baseline");
            check(settled.settles() == 1 && !settled.lastArmed(),
                  "the refusal settles as not armed, as IRadioBackend requires of every outcome");
            check(!session.backend.lastArmRefusalReason().isEmpty(),
                  "and it keeps the sentence the checkbox explains itself with");
            check(autoGainWanted(session.backend.currentOperatingState()),
                  "and the asking survives the refusal, which is the documented intent");
            session.backend.setAutoRfGain(false);
            check(!session.backend.autoRfGainEnabled(),
                  "the switch still reports itself off after the withdrawal");
            check(!autoGainWanted(session.backend.currentOperatingState()),
                  "the withdrawal after a refusal reaches the profile");
            // THE OTHER TWO THINGS A SUCCESSFUL OFF OWES, and the reason the
            // withdrawal now goes through the one disarm branch rather than a
            // hand-copied subset of it. IAutoRfGainControl defines the reason as
            // empty when the last attempt succeeded; an off that took is one.
            check(session.backend.lastArmRefusalReason().isEmpty(),
                  "the reason dies with the request: an off that took is not a declined attempt");
            check(settled.settles() == 2 && !settled.lastArmed(),
                  "the withdrawal settles too -- a view that missed this outcome never refreshed");
            // THE ORDER, PINNED. Sampled inside the handler: emitting before the
            // clear would hand MainWindow::onAutoRfGainArmSettled the stale
            // sentence and re-show the refusal card on a plain off, and every
            // assertion taken after the call returns would still be green.
            check(settled.reasonAtEmit().isEmpty(),
                  "and the reason was already gone when it settled, not merely gone afterwards");
        }
        // THE POSITIVE CONTROL, and it is not optional. The assertion above
        // could pass because the key was never written, because the extension
        // object is empty, because the session failed to build. Running the
        // same assertion against the path that works is what makes the first
        // leg evidence rather than an absence.
        {
            GainSession session(autoGainProfile(false));
            session.backend.setPanRfGain(session.panId, kTrusted);
            session.backend.setAutoRfGain(true);
            check(session.backend.autoRfGainEnabled(),
                  "positive control: a trusted baseline does arm");
            check(autoGainWanted(session.backend.currentOperatingState()),
                  "positive control: and the arm is recorded");
            session.backend.setAutoRfGain(false);
            check(!session.backend.autoRfGainEnabled(),
                  "positive control: it disarms");
            check(!autoGainWanted(session.backend.currentOperatingState()),
                  "positive control: the withdrawal reaches the profile by this path");
        }
        // WHY THE FIRST LEG IS A DEFECT AND NOT BOOKKEEPING. A stranded `true`
        // is what the next connect reads, and the linkUp handler arms on it.
        // Arming from a restored preference is correct in itself; it is the
        // harm only when leg one is what put the `true` there.
        //
        // ILLUSTRATIVE, NOT DISCRIMINATING, AND SAYING SO IS THE POINT. This leg
        // does NOT exercise that connect-time arm and cannot. The handler is a
        // lambda on MetisClient::linkUp, m_metis is private, and GainSession
        // pumps no event loop -- there is no seam here to fire it through, and
        // adding one would mean a harness that drives MetisClient, which is what
        // socket-free costs. What arms below is the explicit setAutoRfGain(true)
        // at a trusted baseline, so this leg passes identically with
        // autoGainProfile(false). Read it as a statement of the harm in code --
        // a restored `true` plus a baseline the loop trusts is exactly the pair
        // that arms -- and read leg one's assertion on the profile as the thing
        // that would go red if the withdrawal regressed.
        {
            GainSession session(autoGainProfile(true));
            session.backend.setPanRfGain(session.panId, kTrusted);
            session.backend.setAutoRfGain(true);
            check(session.backend.autoRfGainEnabled(),
                  "illustrative (not discriminating): a restored autoEnabled:true and a "
                  "trusted baseline are the pair that arms");
        }
        // THE FOURTH LEG: THE PUSH, and it is a different assertion from the three
        // above rather than a restatement of them.
        //
        // Those three ask the backend for its document directly. That read always
        // answers from the live flag, so they stay green on a backend that changes
        // the flag and tells nobody -- which is precisely the state this file was
        // in: setAutoRfGain moved m_autoRfGainWanted on four paths and emitted
        // operatingStateChanged on none of them, so the withdrawal reached the
        // document only for a caller that thought to ask. RadioModel never asks.
        //
        // BOTH HALVES ARE ASSERTED AND NEITHER IS OPTIONAL. Without the refusal's
        // own emit the mirror never records the `true`, and the withdrawal
        // assertion then passes against a document that never said anything at all
        // -- green, on a backend where the withdrawal does not work.
        {
            GainSession session(autoGainProfile(false));
            ProfileMirror mirror(session.backend);
            // A MOVE THAT IS ONE, DERIVED FROM THE LIVE BASELINE RATHER THAN
            // ASSUMED TO DIFFER FROM IT. setPanRfGain notifies on
            // `moved || endedPin || recordedBand`, and this leg is here to prove
            // the MIRROR is wired -- so it has to fire the term it claims to.
            // Setting kUntrusted first would not: the connect baseline is
            // already the shipped default, so `moved` would be false and the
            // save would come from `recordedBand`, the band having had no memory
            // entry. Green today and red on any harness that ever seeds
            // lnaDbByBand, for a reason with nothing to do with auto gain.
            //
            // lnaBaselineDb, not lnaGainDb: the health row named for the gain is
            // lnaEffectiveDb(), baseline plus the automatic offset, and `moved`
            // compares against the baseline.
            const int baselineDb = session.backend.healthSnapshot()
                                       .values.value(QStringLiteral("lnaBaselineDb"), 999)
                                       .toInt();
            const int aRealMove = (baselineDb == kTrusted) ? kTrusted - 1 : kTrusted;
            session.backend.setPanRfGain(session.panId, aRealMove);
            // Also the harness's own positive control: if the mirror were never
            // connected, every assertion below would read a default-constructed
            // document and the false ones would pass for nothing.
            check(baselineDb != aRealMove && mirror.saves() > 0 && !mirror.storedAutoGain(),
                  "push: a gain move does reach the profile, and nothing is wanted yet");
            // AND NOW THE BASELINE THE GUARD REFUSES. Also a move, aRealMove
            // being below the ceiling by construction and kUntrusted above it.
            session.backend.setPanRfGain(session.panId, kUntrusted);
            session.backend.setAutoRfGain(true);
            check(!session.backend.autoRfGainEnabled(),
                  "push: the radio still declines from the shipped default baseline");
            check(mirror.storedAutoGain(),
                  "push: the refusal's surviving ask reaches the PROFILE, not just memory");
            session.backend.setAutoRfGain(false);
            check(!mirror.storedAutoGain(),
                  "push: and the profile the next connect reads now records the operator's off");
        }
        {
            GainSession session(autoGainProfile(false));
            ProfileMirror mirror(session.backend);
            session.backend.setPanRfGain(session.panId, kTrusted);
            session.backend.setAutoRfGain(true);
            check(session.backend.autoRfGainEnabled() && mirror.storedAutoGain(),
                  "push positive control: a trusted baseline arms and the arm is persisted");
            // COUNTED, NOT JUST READ, because `!storedAutoGain()` cannot fail on a
            // backend that tells nobody. If the disarm emits nothing the mirror still
            // holds the document from before the arm, whose autoEnabled was already
            // false -- so the assertion below passes while the disarm it is named for
            // never reached the profile at all. Neither setLnaAutoOffsetDb nor
            // applyBandscopeForAutoGain emits, so nothing else covers this path.
            //
            // THE SAME SHAPE AS THE REFUSAL/WITHDRAWAL PAIR ABOVE. There, a document
            // that never recorded the `true` reads false afterwards either way; here,
            // a document that was never republished reads false either way. Both are
            // an absence being mistaken for a value, and both are fixed by asserting
            // that something was actually said.
            const int savesBeforeDisarm = mirror.saves();
            session.backend.setAutoRfGain(false);
            check(mirror.saves() > savesBeforeDisarm && !mirror.storedAutoGain(),
                  "push positive control: the disarm reaches the profile by the path that worked");
        }
    }
    // Cross-family compatibility at the exact display-restore seam. No Flex or
    // Icom backend is instantiated or changed; their current domain is empty.
    for (const QString& family : {QStringLiteral("flex"), QStringLiteral("icom"),
                                  QStringLiteral("sim")}) {
        int writes = 0;
        const int result = restoreLegacyRfGain(family, false, 20, 7,
            [&writes](int) { ++writes; });
        check(result == 7 && writes == 0, "radio-owned gain retains the existing no-replay behavior");
    }
    int writes = 0;
    check(restoreLegacyRfGain(u"other", true, 20, 7,
              [&writes](int gain) { writes += gain == 20; }) == 20 && writes == 1,
          "another client-owned family retains its existing saved replay");
    check(restoreLegacyRfGain(u"other", true, std::nullopt, 7,
              [&writes](int) { ++writes; }) == 7 && writes == 1,
          "an absent saved gain never writes a default");
    return failures ? 1 : 0;
}
