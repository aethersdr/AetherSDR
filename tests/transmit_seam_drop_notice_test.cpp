// #5637 §1: a TransmitModel control whose value already crossed the
// IRadioBackend seam must not ALSO be reported as dropped.
//
// TransmitModel emits two things for RF power, mic level, the TX passband and
// (#6086) CW speed, break-in, VOX, the SSB monitor and the speech processor
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
//   2. a routed verb on a backend that does not declare the capability behind
//      it still raises commandDropped — the alarm is narrowed, not silenced. So
//      does a verb no seam setter carries (`cw break_in_delay`, an Icom's
//      `vox_delay`).
// Flex declares the same records and has a command plane, so its wire text is
// untouched; the last case pins that its seam setters write nothing.
//
// Socket-free: an injected backend records the seam calls. No radio, no peer.

#include "TestSettingsProfile.h"
#include "core/backends/flex/FlexBackend.h"
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
    QList<int> cwSpeeds;
    QList<bool> cwBreakIns;
    int voxCalls{0};
    int monitorCalls{0};
    int speechProcessorCalls{0};
    QList<int> lineoutGains;
    QList<bool> lineoutMutes;
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
    void setCwSpeed(int wpm) override { cwSpeeds << wpm; }
    void setCwBreakIn(bool on) override { cwBreakIns << on; }
    // Honours tunePowerPercent the way Hl2Backend::setTune does (drive set
    // from TUNE power at key time, PR #4551): records it rather than applying.
    void setTune(bool on, int tunePowerPercent, const TxCoordinator::Operation&,
                 const TxCoordinator::Completion&) override
    {
        tunes << qMakePair(on, tunePowerPercent);
    }
    void setVox(bool, int, int) override { ++voxCalls; }
    void setTxMonitor(bool, int) override { ++monitorCalls; }
    void setLineoutGain(int percent) override { lineoutGains << percent; }
    void setLineoutMute(bool mute) override { lineoutMutes << mute; }
    void setSpeechProcessor(bool, int) override { ++speechProcessorCalls; }
};

// The capability set of a host-modulating transmitter with no command plane —
// the Hermes-Lite 2's answers to the questions the gate asks. The parameter is
// how a NOT-live backend is spelled: no shipped backend answers that way today.
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

// A radio-side CW keyer with no command plane and no host modulation: the
// IC-705 / IC-7300MK2 answers (IcomCivBackend sets hasRadioSideCwKeyer from
// the profile's text keyer).
RadioCapabilities radioSideKeyerTransmitter()
{
    RadioCapabilities c;
    c.family = QStringLiteral("icom");
    c.canTransmit = true;
    c.hasRadioSideCwKeyer = true;
    return c;
}

// An Icom transmitter's answers: its own VOX (no delay register), MON and PROC.
RadioCapabilities icomTransmitter()
{
    RadioCapabilities c = radioSideKeyerTransmitter();
    c.voxControl = RadioCapabilities::VoxControl{/*hasDelay*/ false};
    c.txMonitorControl = RadioCapabilities::TxMonitorControl{};
    c.speechProcessorControl = RadioCapabilities::SpeechProcessorControl{};
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

// A tune state decoded off the radio is not a TUNE this client admitted: with
// no live Tune activity, a tune power change is not forwarded live.
static void tunePowerNotForwardedWithoutTuneActivity()
{
    Fixture f(hostModulatingTransmitter(true));
    check(f.installTxSlice(), "premise: a TX slice is installed");
    TransmitDelta reported;
    reported.tune = true;
    f.radio.transmitModel().applyChanges(reported);
    check(f.radio.transmitModel().isTuning(), "premise: the decoded state reads as tuning");
    check(f.backend->tunes.isEmpty(), "premise: this client never keyed TUNE");
    f.radio.transmitModel().setTunePower(30);
    check(f.backend->tunePowers.isEmpty(),
          "tuning without a live Tune activity: no setTunePower() reaches the backend");
    check(f.droppedStartingWith(QStringLiteral("transmit set tunepower=")),
          "tuning without a live Tune activity: the drop notice stands");
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

static void cwSpeedReachesSeamWithoutDropNotice()
{
    Fixture f(radioSideKeyerTransmitter());
    f.radio.transmitModel().setCwSpeed(25);
    check(f.backend->cwSpeeds == QList<int>{25},
          "cw wpm: setCwSpeed(25) reached the radio-side keyer once");
    check(!f.droppedStartingWith(QStringLiteral("cw wpm ")),
          "cw wpm: no commandDropped for a speed the backend applied");
}

static void cwBreakInReachesSeamWithoutDropNotice()
{
    Fixture f(radioSideKeyerTransmitter());
    f.radio.transmitModel().setCwBreakIn(true);
    check(f.backend->cwBreakIns == QList<bool>{true},
          "cw break_in: setCwBreakIn(true) reached the radio-side keyer once");
    check(!f.droppedStartingWith(QStringLiteral("cw break_in ")),
          "cw break_in: no commandDropped for break-in the backend applied");
}

// No seam setter carries the break-in delay, so its text is a real drop even
// on the backend that takes `cw break_in` (the prefix must not swallow it).
static void cwBreakInDelayKeepsDropNoticeOnKeyerBackend()
{
    Fixture f(radioSideKeyerTransmitter());
    f.radio.transmitModel().setCwDelay(300);
    check(f.droppedStartingWith(QStringLiteral("cw break_in_delay ")),
          "cw break_in_delay: no seam setter, the drop notice stands");
}

// Without a radio-side keyer the setCwSpeed/setCwBreakIn connections hand the
// backend nothing. The speed notice is a real drop. Break-in is a known gap,
// not a ruling: an HL2 reads it from setCwKeying() at key time, which the gate
// does not model, so the notice it keeps there is false.
static void cwSpeedAndBreakInWithoutKeyerKeepDropNotice()
{
    Fixture f(hostModulatingTransmitter());
    f.radio.transmitModel().setCwSpeed(25);
    f.radio.transmitModel().setCwBreakIn(true);
    check(f.backend->cwSpeeds.isEmpty() && f.backend->cwBreakIns.isEmpty(),
          "premise: no radio-side keyer, no CW speed or break-in reaches the seam");
    check(f.droppedStartingWith(QStringLiteral("cw wpm ")),
          "no radio-side keyer: cw wpm still raises commandDropped");
    check(f.droppedStartingWith(QStringLiteral("cw break_in ")),
          "no radio-side keyer: cw break_in still raises commandDropped");
}

static void voxReachesSeamWithoutDropNotice()
{
    Fixture f(icomTransmitter());
    f.radio.transmitModel().setVoxEnable(true);
    f.radio.transmitModel().setVoxLevel(40);
    check(f.backend->voxCalls == 2, "vox: setVox reached the backend for enable and level");
    check(!f.droppedStartingWith(QStringLiteral("transmit set vox_enable="))
              && !f.droppedStartingWith(QStringLiteral("transmit set vox_level=")),
          "vox: no commandDropped for an enable and level the backend applied");
}

// setVox is handed the delay, but a backend without the register ignores it.
static void voxDelayFollowsHasDelay()
{
    Fixture f(icomTransmitter());
    f.radio.transmitModel().setVoxDelay(30);
    check(f.droppedStartingWith(QStringLiteral("transmit set vox_delay=")),
          "vox_delay without a delay register: the drop notice stands");
    RadioCapabilities caps = icomTransmitter();
    caps.voxControl->hasDelay = true;
    Fixture g(caps);
    g.radio.transmitModel().setVoxDelay(30);
    check(!g.droppedStartingWith(QStringLiteral("transmit set vox_delay=")),
          "vox_delay with a delay register: no commandDropped");
}

static void monitorReachesSeamWithoutDropNotice()
{
    Fixture f(icomTransmitter());
    f.radio.transmitModel().setSbMonitor(true);
    f.radio.transmitModel().setMonGainSb(60);
    check(f.backend->monitorCalls == 2,
          "mon: setTxMonitor reached the backend for MON and its gain");
    check(!f.droppedStartingWith(QStringLiteral("transmit set mon="))
              && !f.droppedStartingWith(QStringLiteral("transmit set mon_gain_sb=")),
          "mon: no commandDropped for a monitor the backend applied");
}

static void speechProcessorReachesSeamWithoutDropNotice()
{
    Fixture f(icomTransmitter());
    f.radio.transmitModel().setSpeechProcessorEnable(true);
    f.radio.transmitModel().setSpeechProcessorLevel(1);
    check(f.backend->speechProcessorCalls == 2,
          "proc: setSpeechProcessor reached the backend for enable and level");
    check(!f.droppedStartingWith(QStringLiteral("transmit set speech_processor_")),
          "proc: no commandDropped for a processor the backend applied");
}

// A host-modulating transmitter (HL2, ANAN) runs PROC in this host's ClientComp
// (MainWindow::applySpeechProcessorToClientComp), not through the seam setter.
static void speechProcessorOnHostCompressorWithoutDropNotice()
{
    Fixture f(hostModulatingTransmitter());
    f.radio.transmitModel().setSpeechProcessorEnable(true);
    f.radio.transmitModel().setSpeechProcessorLevel(2);
    check(f.backend->speechProcessorCalls == 0,
          "premise: no PROC record, so the seam setter is not called");
    check(!f.droppedStartingWith(QStringLiteral("transmit set speech_processor_")),
          "proc on a host-modulating transmitter: no commandDropped");
}

// Neither the record nor a transmitter whose host compressor could serve it.
static void speechProcessorWithNoProcessorKeepsDropNotice()
{
    RadioCapabilities caps = hostModulatingTransmitter();
    caps.canTransmit = false;
    Fixture f(caps);
    f.radio.transmitModel().setSpeechProcessorEnable(true);
    check(f.backend->speechProcessorCalls == 0,
          "premise: no PROC record, so the seam setter is not called");
    check(f.droppedStartingWith(QStringLiteral("transmit set speech_processor_enable=")),
          "no radio or host processor: speech_processor still raises commandDropped");
}

// The HL2 implements neither setVox nor setTxMonitor and declares neither record.
static void voxAndMonitorWithoutRecordsKeepDropNotice()
{
    Fixture f(hostModulatingTransmitter());
    f.radio.transmitModel().setVoxEnable(true);
    f.radio.transmitModel().setVoxLevel(40);
    f.radio.transmitModel().setSbMonitor(true);
    f.radio.transmitModel().setMonGainSb(60);
    check(f.backend->voxCalls == 0 && f.backend->monitorCalls == 0,
          "premise: without the records neither setter is called");
    check(f.droppedStartingWith(QStringLiteral("transmit set vox_enable="))
              && f.droppedStartingWith(QStringLiteral("transmit set vox_level=")),
          "no VOX record: vox still raises commandDropped");
    check(f.droppedStartingWith(QStringLiteral("transmit set mon="))
              && f.droppedStartingWith(QStringLiteral("transmit set mon_gain_sb=")),
          "no monitor record: mon still raises commandDropped");
}

// Flex now has its VOX, monitor and PROC setters called (it declares the
// records), and they must write nothing: the wire text from TransmitModel is
// still the only Flex output for these controls.
// The title-bar speaker mutes the radio's line out and the PC sink together
// (#4665). Without a command plane the line out goes through the seam, and its
// Flex text must not raise a drop notice for a request the seam carried.
static void lineoutReachesSeamWithoutDropNotice()
{
    Fixture f(hostModulatingTransmitter());
    f.radio.setLineoutMute(true);
    f.radio.setLineoutMute(false);
    f.radio.setLineoutGain(30);
    check(f.backend->lineoutMutes == QList<bool>{true, false},
          "lineout mute: setLineoutMute() reached the backend for each click");
    check(f.backend->lineoutGains == QList<int>{30},
          "lineout gain: setLineoutGain(30) reached the backend once");
    check(!f.droppedStartingWith(QStringLiteral("mixer lineout")),
          "lineout: no commandDropped on a backend without a command plane");
}

static void flexSeamSettersWriteNothing()
{
    FlexBackend flex;
    QStringList written;
    flex.setCommandSink([&written](const QString& cmd) { written << cmd; });
    flex.setSliceCommandSink([&written](const QString& cmd) { written << cmd; });
    flex.setTxCommandSink([&written](const QString& cmd, const TxCoordinator::Command&) {
        written << cmd;
    });
    const RadioCapabilities caps = flex.capabilities();
    check(caps.voxControl && caps.voxControl->hasDelay && caps.txMonitorControl
              && caps.speechProcessorControl,
          "flex: declares VOX (with delay), monitor and PROC");
    flex.setVox(true, 40, 30);
    flex.setTxMonitor(true, 60);
    flex.setSpeechProcessor(true, 1);
    check(written.isEmpty(), "flex: setVox/setTxMonitor/setSpeechProcessor write nothing");

    TransmitModel model;
    QStringList text;
    QObject::connect(&model, &TransmitModel::commandReady,
                     [&text](const QString& cmd) { text << cmd; });
    model.setVoxEnable(true);
    model.setVoxDelay(30);
    model.setSbMonitor(true);
    model.setSpeechProcessorLevel(1);
    check(text == QStringList{QStringLiteral("transmit set vox_enable=1"),
                              QStringLiteral("transmit set vox_delay=30"),
                              QStringLiteral("transmit set mon=1"),
                              QStringLiteral("transmit set speech_processor_level=1")},
          "flex: the wire text for these controls is unchanged");
}

// The negative control that keeps the routed cases honest: the Icom-shaped
// transmitter they use, and a verb nothing behind the seam implements. If the fix had
// gated the whole commandReady forward, this is what would go quiet.
static void unroutedVerbStillRaisesDropNotice()
{
    Fixture f(icomTransmitter());
    f.radio.transmitModel().setMonGainCw(50);
    check(f.droppedStartingWith(QStringLiteral("transmit set mon_gain_cw=")),
          "mon_gain_cw: a verb with no seam setter still raises commandDropped");
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
    tunePowerNotForwardedWithoutTuneActivity();
    tunePowerChangedWhileKeyedKeepsDropNotice();
    cwPitchNeverHandedToBackendKeepsDropNotice();
    cwPitchHandedToPreviousBackendKeepsDropNotice();
    cwPitchDifferentFromHandedValueKeepsDropNotice();
    cwPitchRepeatOfHandedValueStaysQuiet();
    cwSpeedReachesSeamWithoutDropNotice();
    cwBreakInReachesSeamWithoutDropNotice();
    cwBreakInDelayKeepsDropNoticeOnKeyerBackend();
    cwSpeedAndBreakInWithoutKeyerKeepDropNotice();
    voxReachesSeamWithoutDropNotice();
    voxDelayFollowsHasDelay();
    monitorReachesSeamWithoutDropNotice();
    speechProcessorReachesSeamWithoutDropNotice();
    speechProcessorOnHostCompressorWithoutDropNotice();
    speechProcessorWithNoProcessorKeepsDropNotice();
    voxAndMonitorWithoutRecordsKeepDropNotice();
    lineoutReachesSeamWithoutDropNotice();
    flexSeamSettersWriteNothing();
    unroutedVerbStillRaisesDropNotice();
    undeclaredCapabilityKeepsDropNotice();
    tunePowerWithoutDriveOwnershipKeepsDropNotice();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
