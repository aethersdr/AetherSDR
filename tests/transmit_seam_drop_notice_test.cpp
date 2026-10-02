// #5637 §1: a TransmitModel control whose value already crossed the
// IRadioBackend seam must not ALSO be reported as dropped.
//
// TransmitModel emits two things for RF power, mic level and the TX passband
// (tune power reaches the backend as setTune()'s argument at key time, and
// while keyed through setTunePower() where the backend declares it live):
// a typed intent (rfPowerCommandIssued / micLevelCommandIssued /
// txFilterCommandIssued) that RadioModel hands to the backend, and the legacy
// Flex wire text through commandReady. On a backend with no command plane the
// wire text reached RadioModel::sendCmd, which logged "no command plane for
// this backend, dropping transmit set rfpower=N" and emitted commandDropped —
// while the backend had just applied the value. The report on #5637 was aimed
// at that line, and the one-shot "nothing was sent to the radio" notice it
// raises was consumed by a control that works.
//
// The drop notice is deliberate (#5263: it is how dead controls on non-Flex
// radios are found), so this test pins BOTH halves:
//   1. a routed verb reaches the backend and raises no commandDropped;
//   2. an unrouted verb (VOX here, which this backend does not implement) and a
//      routed verb on a backend that does not declare the capability behind it
//      still raise commandDropped — the alarm is narrowed, not silenced.
//
// Socket-free: an injected backend records the seam calls. No radio, no peer.

#include "TestSettingsProfile.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>
#include <memory>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool ok, const char* message)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}

class RecordingBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    QList<int> txPowers;
    QList<int> micGains;
    QList<QPair<int, int>> txFilters;
    QList<int> cwPitches;
    QList<QPair<bool, int>> tunes;  // (on, tunePowerPercent) per setTune()
    QList<int> tunePowers;          // per setTunePower()
    bool connected{true};
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
    void setTxPower(int percent) override { txPowers << percent; }
    void setMicGain(int level) override { micGains << level; }
    void setTxFilter(int lowHz, int highHz) override { txFilters << qMakePair(lowHz, highHz); }
    void setCwPitch(int hz) override { cwPitches << hz; }
    void setTunePower(int percent) override { tunePowers << percent; }
    // Honours tunePowerPercent the way Hl2Backend::setTune does (drive set
    // from TUNE power at key time, PR #4551): records it rather than applying.
    void setTune(bool on, int tunePowerPercent, const TxCoordinator::Operation&,
                 const TxCoordinator::Completion&) override
    {
        tunes << qMakePair(on, tunePowerPercent);
    }
    // setVox() is deliberately NOT overridden: this backend has no VOX, so the
    // Flex text for it really does reach nothing.
};

// The capability set of a host-modulating transmitter with no command plane —
// the Hermes-Lite 2's answers to the questions the gate asks.
RadioCapabilities hostModulatingTransmitter(bool tunePowerAppliesLive = true)
{
    RadioCapabilities c;
    c.family = QStringLiteral("hl2");
    c.canTransmit = true;
    c.hostModulates = true;
    c.transmitDriveControl = RadioCapabilities::TransmitDriveControl{
        SliceFrequencyControl::Authority::Engine, tunePowerAppliesLive};
    c.hasTxFilterControls = true;
    return c;
}

struct Fixture {
    RadioModel radio;
    RecordingBackend* backend{nullptr};
    QStringList dropped;

    explicit Fixture(const RadioCapabilities& caps)
    {
        auto owned = std::make_unique<RecordingBackend>();
        backend = owned.get();
        backend->caps = caps;
        radio.setBackendForTest(std::move(owned), caps.family);
        QObject::connect(&radio, &RadioModel::commandDropped, &radio,
                         [this](const QString& cmd) { dropped << cmd; });
    }

    // TUNE resolves through txSlice(); install one the way radio status would.
    // The socket-free slice fixture is refused while connected, so the link is
    // down only for the install.
    bool installTxSlice()
    {
        backend->connected = false;
        const bool installed = radio.automationApplySliceFixture(0, QStringLiteral("A"));
        backend->connected = true;
        if (!installed) {
            return false;
        }
        SliceModel* slice = radio.slice(0);
        if (!slice) {
            return false;
        }
        SliceDelta delta;
        delta.txSlice = true;
        delta.panId = QStringLiteral("0x40000000");
        slice->applyChanges(delta);
        return radio.txSlice() == slice;
    }

    bool droppedStartingWith(const QString& prefix) const
    {
        for (const QString& cmd : dropped) {
            if (cmd.startsWith(prefix)) {
                return true;
            }
        }
        return false;
    }
};
} // namespace

static void premiseHasNoCommandPlane()
{
    Fixture f(hostModulatingTransmitter());
    check(!f.radio.hasCommandPlane(),
          "premise: the injected non-Flex backend has no command plane");
}

static void rfPowerReachesSeamWithoutDropNotice()
{
    Fixture f(hostModulatingTransmitter());
    f.radio.transmitModel().setRfPower(90);
    check(f.backend->txPowers == QList<int>{90},
          "rfpower: setTxPower(90) reached the backend exactly once");
    check(!f.droppedStartingWith(QStringLiteral("transmit set rfpower=")),
          "rfpower: no commandDropped for a value the backend applied");
}

static void micLevelReachesSeamWithoutDropNotice()
{
    Fixture f(hostModulatingTransmitter());
    f.radio.transmitModel().setMicLevel(42);
    check(f.backend->micGains == QList<int>{42},
          "miclevel: setMicGain(42) reached the backend exactly once");
    check(!f.droppedStartingWith(QStringLiteral("transmit set miclevel=")),
          "miclevel: no commandDropped for a value the backend applied");
}

static void txFilterReachesSeamWithoutDropNotice()
{
    Fixture f(hostModulatingTransmitter());
    f.radio.transmitModel().setTxFilter(200, 2800);
    check(f.backend->txFilters.size() == 1
              && f.backend->txFilters.first() == qMakePair(200, 2800),
          "filter: setTxFilter(200, 2800) reached the backend exactly once");
    check(!f.droppedStartingWith(QStringLiteral("transmit set filter_low=")),
          "filter: no commandDropped for a passband the backend applied");
}

static void cwPitchReachesSeamWithoutDropNotice()
{
    Fixture f(hostModulatingTransmitter());
    f.radio.transmitModel().setCwPitch(700);
    check(f.backend->cwPitches == QList<int>{700},
          "cw pitch: setCwPitch(700) reached the host-modulating backend once");
    check(!f.droppedStartingWith(QStringLiteral("cw pitch ")),
          "cw pitch: no commandDropped for a pitch the backend applied");
}

// Unkeyed, tune power has nothing to re-apply: setTune() hands tunePower() to
// the backend at key time (#4551), so the text is not a drop and no
// setTunePower() is sent.
static void tunePowerDeliveredAtKeyTimeWithoutDropNotice()
{
    Fixture f(hostModulatingTransmitter());
    check(f.installTxSlice(), "premise: a TX slice is installed");
    f.radio.transmitModel().setTunePower(25);
    check(!f.droppedStartingWith(QStringLiteral("transmit set tunepower=")),
          "tunepower: no commandDropped on a backend that applies it at key time");
    check(f.backend->tunePowers.isEmpty(),
          "tunepower while not tuning: no setTunePower() reaches the backend");
    f.radio.transmitModel().startTune();
    check(!f.backend->tunes.isEmpty() && f.backend->tunes.first() == qMakePair(true, 25),
          "tunepower: TUNE keyed with setTune(true, 25), the slider's value");
    f.radio.transmitModel().stopTune();
}

// A backend declaring tunePowerAppliesLive takes a mid-carrier change through
// setTunePower(), without re-keying, so the text is not a drop.
static void tunePowerChangedWhileKeyedAppliesLive()
{
    Fixture f(hostModulatingTransmitter(true));
    check(f.installTxSlice(), "premise: a TX slice is installed");
    f.radio.transmitModel().setTunePower(10);
    f.radio.transmitModel().startTune();
    check(f.radio.transmitModel().isTuning(), "premise: TUNE is keyed");
    const auto tunesAtKeyDown = f.backend->tunes;
    f.dropped.clear();
    f.radio.transmitModel().setTunePower(30);
    check(f.backend->tunePowers == QList<int>{30},
          "live tune power: setTunePower(30) reached the backend exactly once");
    check(f.backend->tunes == tunesAtKeyDown,
          "live tune power: the change does not re-key TUNE");
    check(!f.droppedStartingWith(QStringLiteral("transmit set tunepower=")),
          "live tune power: no commandDropped for a value the backend applied");

    // A decoded status of a different value is the radio's report, not
    // operator intent (the path RadioModel::applyBackendTransmitDelta takes).
    TransmitDelta echo;
    echo.tunePower = 45;
    f.radio.transmitModel().applyChanges(echo);
    check(f.radio.transmitModel().tunePower() == 45,
          "premise: the decoded tune power reached TransmitModel");
    check(f.backend->tunePowers == QList<int>{30},
          "a decoded tune power while keyed sends no setTunePower()");

    f.radio.transmitModel().stopTune();
    f.radio.transmitModel().setTunePower(50);
    check(f.backend->tunePowers == QList<int>{30},
          "after TUNE is released: no setTunePower() reaches the backend");
}

// Without tunePowerAppliesLive, a change while TUNE is keyed reaches no seam
// setter, so its text is a real drop.
static void tunePowerChangedWhileKeyedKeepsDropNotice()
{
    Fixture f(hostModulatingTransmitter(false));
    check(f.installTxSlice(), "premise: a TX slice is installed");
    f.radio.transmitModel().setTunePower(10);
    f.radio.transmitModel().startTune();
    check(f.radio.transmitModel().isTuning(), "premise: TUNE is keyed");
    const auto tunesAtKeyDown = f.backend->tunes;
    f.dropped.clear();
    f.radio.transmitModel().setTunePower(30);
    check(f.backend->tunePowers.isEmpty(),
          "not live: no setTunePower() reaches a backend that does not declare it");
    check(f.backend->tunes == tunesAtKeyDown,
          "not live: a mid-carrier tune power change does not re-key TUNE");
    check(f.droppedStartingWith(QStringLiteral("transmit set tunepower=")),
          "tunepower changed while TUNE is keyed, not declared live: the drop notice stands");
    f.radio.transmitModel().stopTune();
}

// TransmitModel::setCwPitch emits `cw pitch N` on every call but
// cwPitchChanged only on a change, and the host-modulating seam connection is
// the change-gated one. A set that repeats the model's value therefore hands
// the backend nothing. Withholding the notice is right only if THIS backend
// was already handed that value; if it never was, the text is a real drop.
static void cwPitchNeverHandedToBackendKeepsDropNotice()
{
    Fixture f(hostModulatingTransmitter());
    const int pitch = f.radio.transmitModel().cwPitch();  // the model default
    f.radio.transmitModel().setCwPitch(pitch);
    check(f.backend->cwPitches.isEmpty(),
          "premise: a repeat of the model's pitch reaches no seam setter");
    check(f.droppedStartingWith(QStringLiteral("cw pitch ")),
          "cw pitch never handed to this backend: the drop notice stands");
}

// ...and the same after a backend swap: the old backend held the value, the
// new one has never been handed anything.
static void cwPitchHandedToPreviousBackendKeepsDropNotice()
{
    Fixture f(hostModulatingTransmitter());
    f.radio.transmitModel().setCwPitch(700);
    auto fresh = std::make_unique<RecordingBackend>();
    RecordingBackend* second = fresh.get();
    second->caps = hostModulatingTransmitter();
    f.radio.setBackendForTest(std::move(fresh), second->caps.family);
    f.backend = second;
    f.dropped.clear();
    f.radio.transmitModel().setCwPitch(700);
    check(second->cwPitches.isEmpty(),
          "premise: the repeat reaches no setter on the new backend");
    check(f.droppedStartingWith(QStringLiteral("cw pitch ")),
          "pitch handed only to the previous backend: the drop notice stands");
}

// The value must match, not merely exist: a backend that holds 700 and is
// handed nothing when the model moves to 800 (its capabilities stopped routing
// the pitch) has not received 800.
static void cwPitchDifferentFromHandedValueKeepsDropNotice()
{
    Fixture f(hostModulatingTransmitter());
    f.radio.transmitModel().setCwPitch(700);
    f.backend->caps.hostModulates = false;
    f.dropped.clear();
    f.radio.transmitModel().setCwPitch(800);
    check(f.backend->cwPitches == QList<int>{700},
          "premise: 800 reaches no setter once the pitch is not routed");
    check(f.droppedStartingWith(QStringLiteral("cw pitch 800")),
          "pitch other than the one this backend holds: the drop notice stands");
}

// The control for the two above: a repeat of a value this backend WAS handed
// (an operator tabbing out of an unchanged pitch field) stays quiet — the
// backend holds exactly what the text carries.
static void cwPitchRepeatOfHandedValueStaysQuiet()
{
    Fixture f(hostModulatingTransmitter());
    f.radio.transmitModel().setCwPitch(700);
    f.radio.transmitModel().setCwPitch(700);
    check(f.backend->cwPitches == QList<int>{700},
          "cw pitch: the backend was handed 700 once");
    check(!f.droppedStartingWith(QStringLiteral("cw pitch ")),
          "cw pitch: a repeat of the value the backend holds raises no notice");
}

// The negative control that keeps the first four honest: the same backend,
// the same model, a verb nothing behind the seam implements. If the fix had
// gated the whole commandReady forward, this is what would go quiet.
static void unroutedVerbStillRaisesDropNotice()
{
    Fixture f(hostModulatingTransmitter());
    f.radio.transmitModel().setVoxEnable(true);
    check(f.droppedStartingWith(QStringLiteral("transmit set vox_enable=")),
          "vox: an unrouted verb still raises commandDropped");
}

// Routed on the seam is not enough: the backend must also declare the
// capability that says the setter does something. An RX-only host-DSP backend
// (the ANAN today: drive ownership declared, canTransmit false, setTxPower not
// implemented) must keep its alarm.
static void undeclaredCapabilityKeepsDropNotice()
{
    RadioCapabilities caps = hostModulatingTransmitter();
    caps.canTransmit = false;
    caps.hasTxFilterControls = false;
    caps.hostModulates = false;
    Fixture f(caps);
    f.radio.transmitModel().setRfPower(80);
    check(f.droppedStartingWith(QStringLiteral("transmit set rfpower=")),
          "receive-only backend: rfpower still raises commandDropped");
    f.radio.transmitModel().setMicLevel(30);
    check(f.droppedStartingWith(QStringLiteral("transmit set miclevel=")),
          "receive-only backend: miclevel still raises commandDropped");
    f.radio.transmitModel().setTxFilter(300, 2700);
    check(f.droppedStartingWith(QStringLiteral("transmit set filter_low=")),
          "no TX filter controls declared: the passband still raises commandDropped");
    f.radio.transmitModel().setCwPitch(650);
    check(f.droppedStartingWith(QStringLiteral("cw pitch ")),
          "no host CW demod or radio keyer: cw pitch still raises commandDropped");
    f.radio.transmitModel().setTunePower(20);
    check(f.droppedStartingWith(QStringLiteral("transmit set tunepower=")),
          "receive-only backend: tunepower still raises commandDropped");
}

// A backend that can key but does not own its drive has nothing to apply a
// tune power WITH: setTune()'s argument is ignored there, so the text is a
// real drop and must stay loud.
static void tunePowerWithoutDriveOwnershipKeepsDropNotice()
{
    RadioCapabilities caps = hostModulatingTransmitter();
    caps.transmitDriveControl.reset();
    Fixture f(caps);
    f.radio.transmitModel().setTunePower(30);
    check(f.droppedStartingWith(QStringLiteral("transmit set tunepower=")),
          "no drive ownership declared: tunepower still raises commandDropped");
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("transmit-seam-drop-notice"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    premiseHasNoCommandPlane();
    rfPowerReachesSeamWithoutDropNotice();
    micLevelReachesSeamWithoutDropNotice();
    txFilterReachesSeamWithoutDropNotice();
    cwPitchReachesSeamWithoutDropNotice();
    tunePowerDeliveredAtKeyTimeWithoutDropNotice();
    tunePowerChangedWhileKeyedAppliesLive();
    tunePowerChangedWhileKeyedKeepsDropNotice();
    cwPitchNeverHandedToBackendKeepsDropNotice();
    cwPitchHandedToPreviousBackendKeepsDropNotice();
    cwPitchDifferentFromHandedValueKeepsDropNotice();
    cwPitchRepeatOfHandedValueStaysQuiet();
    unroutedVerbStillRaisesDropNotice();
    undeclaredCapabilityKeepsDropNotice();
    tunePowerWithoutDriveOwnershipKeepsDropNotice();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
