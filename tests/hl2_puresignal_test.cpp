// HL2 PureSignal: the controller, the feedback-gain step, and the wire facts
// they lean on — the 0x0a PS bit, the keyed feedback gain and the four-DDC
// floor. Clock-free and socket-free: the controller is driven report by
// report with the GetPSInfo values calcc would give.

#include "core/backends/hl2/Hl2PureSignal.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>

#include <cstdio>

namespace AetherSDR::hl2 {
struct MetisClientTestAccess {
    // The board's receiver count arrives with start()'s Params; set it alone.
    static void setBoardMaxRx(MetisClient& c, int n) { c.m_params.boardMaxRx = n; }
};
}  // namespace AetherSDR::hl2

using AetherSDR::hl2::Hl2PureSignal;
using AetherSDR::hl2::Hl2PsController;
using AetherSDR::hl2::MetisClient;
using AetherSDR::hl2::MetisClientTestAccess;
namespace hl2 = AetherSDR::hl2;
using L = AetherSDR::hl2::Hl2PsController;

namespace {

int g_failures = 0;
void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

using Ctl = Hl2PsController::Flags;
using Mode = Hl2PsController::Mode;
using Report = Hl2PsController::Report;
using Gain = hl2::Hl2PsGain;

constexpr Ctl kOff{true, false, false, false};
constexpr Ctl kAuto{true, false, true, false};
constexpr Ctl kSingle{true, true, false, false};
constexpr Ctl kTurnOn{false, false, false, true};

Report at(int state, int attempts = 0, int fb = 0, bool correcting = false, bool fresh = true)
{
    Report r;
    r.state = state;
    r.attempts = attempts;
    r.feedbackLevel = fb;
    r.correcting = correcting;
    r.feedbackFresh = fresh;
    return r;
}

void gainStep()
{
    check(Gain::stepDb(152) == 0, "152 is on the centre");
    check(Gain::stepDb(0) == -Gain::kMaxStepDb, "no feedback: one full step more gain");
    check(Gain::stepDb(300) == Gain::kMaxStepDb, "above the scale: one full step less");
    check(Gain::stepDb(76) == -6, "half the centre is -6 dB");
    check(Gain::stepDb(256) == 5, "full scale is 4.5 dB hot, rounds to 5");
    check(Gain::stepDb(30) == -Gain::kMaxStepDb, "-14 dB is held to one full step");
    check(Gain::stepDb(200) == 2, "200 is 2.4 dB hot");

    check(Gain::inWindow(129) && !Gain::inWindow(128) && Gain::inWindow(181)
              && !Gain::inWindow(182),
          "window is 128 < fb <= 181");
    check(Gain::clampAtt(-40) == -28 && Gain::clampAtt(40) == 31,
          "attenuation clamps to -28..31");
    check(Gain::limit(31, 200) == 1 && Gain::limit(31, 181) == 0 && Gain::limit(30, 250) == 0,
          "too strong only at the most attenuation with the level above the window");
    check(Gain::limit(-28, 100) == -1 && Gain::limit(-28, 0) == 0 && Gain::limit(-27, 100) == 0,
          "too weak only at full gain with a measured level below the window");
    check(Gain::lnaGainDbForAtt(31) == -12 && Gain::lnaGainDbForAtt(-28) == 47,
          "gain is 19 - att, inside the LNA's -12..+48 dB");
}

void limitNeedsALevelMeasuredHere()
{
    std::printf("
  the limit belongs to the attenuation the level was measured at
");
    AetherSDR::hl2::Hl2PsMeasuredAt m;
    check(m.limit(31, 220) == 0, "nothing measured yet: no limit");
    // Auto-attenuate at 28 reads 220; the controller steps to 31.
    m.observe(true, 1, 28);
    check(m.limit(28, 220) == 0, "at 28 a hot level is not yet a limit");
    check(m.limit(31, 220) == 0, "stepped onto 31: the 28 dB level is no +1");
    m.observe(true, 1, 31);
    check(m.limit(31, 220) == 0, "the same attempt read again does not count as measured at 31");
    m.observe(true, 2, 31);
    check(m.limit(31, 220) == 1, "a fresh attempt at 31 still too strong: +1");
    // Hold: calcc measures no more; the operator drags the slider.
    check(m.limit(30, 220) == 0, "a manual move away drops the warning");
    check(m.limit(31, 220) == 1, "and back to where it was measured restores it");
    m.observe(false, 3, 31);
    check(m.limit(31, 220) == 1, "an attempt that measured nothing keeps the last level's att");
    m.observe(true, 4, -28);
    check(m.limit(-28, 100) == -1, "a fresh weak level at full gain: -1");
    m.reset();
    check(m.limit(-28, 100) == 0, "reset forgets it");
}

void autoOnAndOff()
{
    Hl2PsController c;
    c.setAutoAttenuate(false);
    check(c.mode() == Mode::Off && !c.step(at(L::LReset)).flags, "starts Off, sends nothing");
    c.selectAuto();
    auto s = c.step(at(L::LReset));
    check(s.flags == kAuto, "Auto: reset + automode");
    check(c.mode() == Mode::Auto && c.calibrating(), "Auto is calibrating");
    s = c.step(at(L::LCollect));
    check(!s.flags && !s.attenuationDb, "Auto leaves calcc to it");
    c.selectOff();
    s = c.step(at(L::LCollect, 0, 0, true));
    check(s.flags == kOff && c.mode() == Mode::Off && !c.calibrating(), "Off: reset alone");
    check(!c.step(at(L::LReset)).flags, "Off sends nothing more");
}

void singleCalibration()
{
    Hl2PsController c;
    c.setAutoAttenuate(false);
    c.selectSingle();
    check(c.step(at(L::LReset)).flags == kSingle, "Single: reset + mancal");
    check(!c.step(at(L::LWait)).flags && c.mode() == Mode::Single, "waits for the key");
    c.step(at(L::LCollect));
    c.step(at(L::LCalc));
    check(c.mode() == Mode::Single && c.calibrating(), "still Single while calcc works");
    c.step(at(L::LStayOn, 1, 150, true));
    check(c.mode() == Mode::Hold && !c.calibrating(), "correction held -> Hold");
    check(!c.step(at(L::LStayOn, 1, 150, true)).flags, "Hold sends nothing");
}

void singleGivesUp()
{
    Hl2PsController c;
    c.setAutoAttenuate(false);
    c.selectSingle();
    c.step(at(L::LReset));
    check(!c.step(at(L::LReset)).flags, "LRESET before calcc moved is not a failure");
    for (int start = 2; start <= Hl2PsController::kSingleAttempts; ++start) {
        c.step(at(L::LCollect));
        const auto s = c.step(at(L::LReset));
        check(s.flags == kSingle && c.mode() == Mode::Single, "calcc gave up: Single again");
    }
    c.step(at(L::LCollect));
    const auto s = c.step(at(L::LReset));
    check(!s.flags && c.mode() == Mode::Off, "out of attempts: Off");
}

void restoring()
{
    Hl2PsController c;
    c.selectAuto();
    c.step(at(L::LReset));
    c.selectRestoring();
    auto s = c.step(at(L::LCollect));
    check(s.flags == kTurnOn, "Restore: turnon alone, clears automode");
    check(c.mode() == Mode::Restoring && !c.calibrating(), "Restoring is not calibrating");
    c.step(at(L::LStayOn, 0, 0, true));
    check(c.mode() == Mode::Hold, "loaded correction held -> Hold");
}

void autoAttenuate()
{
    Hl2PsController c;
    check(c.attenuationDb() == 31 && c.autoAttenuate(), "starts at the lowest gain, on");
    c.selectAuto();
    c.step(at(L::LReset, 4));          // flags out; attempts 4 is the baseline

    // An attempt that lands before calcc collected again is from before the
    // flags: not judged.
    auto s = c.step(at(L::LWait, 5, 30));
    check(!s.attenuationDb && !s.flags, "attempt from before the flags is ignored");

    c.step(at(L::LCollect, 5, 30));
    s = c.step(at(L::LCollect, 6, 30));
    check(s.attenuationDb && *s.attenuationDb == 21, "too cold: 31 - 10 = 21");
    check(s.flags == kAuto, "and Auto starts over at the new gain");
    check(c.attenuationDb() == 21, "controller holds 21");

    s = c.step(at(L::LWait, 7, 30));
    check(!s.attenuationDb, "next attempt only counts once calcc collected again");
    c.step(at(L::LMoxDelay, 7, 30));
    s = c.step(at(L::LCollect, 7, 30));
    check(!s.attenuationDb, "same attempt count: nothing new");
    s = c.step(at(L::LCollect, 8, 30, false, false));
    check(!s.attenuationDb, "level not measured: nothing to judge");
    s = c.step(at(L::LCollect, 9, 150));
    check(!s.attenuationDb && !s.flags, "in the window: no move");
    s = c.step(at(L::LCollect, 10, 200));
    check(s.attenuationDb && *s.attenuationDb == 23, "2 dB hot: 21 + 2");

    Hl2PsController top;
    top.setAttenuationDb(Gain::kAttMin);
    top.selectAuto();
    top.step(at(L::LReset));
    top.step(at(L::LCollect));
    s = top.step(at(L::LCollect, 1, 30));
    check(!s.attenuationDb && !s.flags, "at full gain a cold level has nowhere to go");

    Hl2PsController manual;
    manual.setAutoAttenuate(false);
    manual.selectAuto();
    manual.step(at(L::LReset));
    manual.step(at(L::LCollect));
    s = manual.step(at(L::LCollect, 1, 30));
    check(!s.attenuationDb && !s.flags && manual.attenuationDb() == 31,
          "auto-attenuate off leaves the gain alone");

    Hl2PsController held;
    held.setAutoAttenuate(false);
    held.selectSingle();
    held.step(at(L::LReset));
    held.step(at(L::LCollect));
    held.step(at(L::LStayOn, 1, 150, true));
    held.setAutoAttenuate(true);
    s = held.step(at(L::LStayOn, 2, 30, true));
    check(held.mode() == Mode::Hold && !s.attenuationDb, "Hold never moves the gain");
}

void singleGainMovesBounded()
{
    Hl2PsController c;
    c.setAttenuationDb(0);
    c.selectSingle();
    c.step(at(L::LReset));
    int attempts = 0;
    int moves = 0;
    for (int i = 0; i < 2 * Hl2PsController::kSingleGainMoves; ++i) {
        c.step(at(L::LCollect, attempts));
        // Swing across the window: too hot, then too cold.
        const auto s = c.step(at(L::LCollect, ++attempts, (i % 2) ? 60 : 250));
        if (s.attenuationDb) {
            ++moves;
            check(s.flags == kSingle, "a gain move starts Single over");
        }
    }
    check(moves == Hl2PsController::kSingleGainMoves,
          "Single moves the gain a bounded number of times");
    check(c.mode() == Mode::Single, "and is left to finish");
}

void controllerAutoSwitch()
{
    // The switch the controller reports follows Single and Reset. No TX DSP:
    // these paths never reach the channel.
    Hl2PureSignal ps(nullptr);
    bool autoReported = false;
    bool singleReported = false;
    int reports = 0;
    QObject::connect(&ps, &Hl2PureSignal::statusChanged,
                     [&](const hl2::Hl2PureSignalStatus& s) {
                         autoReported = s.autoCalibrate;
                         singleReported = s.singleCalibrating;
                         ++reports;
                     });
    ps.setAvailability(true, QString());
    ps.setAutoCalibrate(true);
    check(autoReported, "Auto reported on");
    const int before = reports;
    ps.calibrateOnce();
    check(reports > before && !autoReported, "Single reports Auto off");
    check(!singleReported, "PureSignal off: Single not reported lit");
    ps.setAutoCalibrate(true);
    ps.resetCorrection();
    check(!autoReported, "Reset reports Auto off");
}

void receiverCountUnderFloor()
{
    // PureSignal's floor keeps four DDCs on the wire; a receiver change made
    // under it must still be the count once PureSignal turns off.
    {
        MetisClient c;
        MetisClientTestAccess::setBoardMaxRx(c, 4);
        c.setPureSignal(true);
        check(c.effectiveNumRx() == 4, "PS on: four DDCs");
        c.setReceiverCount(2);
        check(c.effectiveNumRx() == 4, "second receiver under the floor: still four");
        c.setPureSignal(false);
        check(c.effectiveNumRx() == 2, "PS off keeps the receiver added under it");
    }
    {
        MetisClient c;
        MetisClientTestAccess::setBoardMaxRx(c, 4);
        c.setReceiverCount(2);
        c.setPureSignal(true);
        c.setReceiverCount(1);
        c.setPureSignal(false);
        check(c.effectiveNumRx() == 1, "PS off keeps the receiver removed under it");
    }
}

void wire()
{
    const hl2::Cc plain = hl2::ccGain(10, false);
    const hl2::Cc ps = hl2::ccGain(10, true);
    check(plain == hl2::ccRxGain(10), "PS off is the plain gain bank");
    check((ps[2] & hl2::kGainPureSignal) != 0 && (plain[2] & hl2::kGainPureSignal) == 0,
          "PS sets 0x0a C2 bit 6");
    check(ps[4] == plain[4], "PS bit leaves the gain byte");
    check(hl2::ccGain(hl2::Hl2PsGain::lnaGainDbForAtt(31), true)[4] == 0x40,
          "att 31 is ((31 - 31) & 0x3F) | 0x40");
    check(hl2::ccGain(hl2::Hl2PsGain::lnaGainDbForAtt(0), true)[4] == (0x40 | 31),
          "att 0 is ((31 - 0) & 0x3F) | 0x40");

    MetisClient::Params p;
    p.numRx = 1;
    p.boardMaxRx = 4;
    check(MetisClient::effectiveNumRx(p) == 1, "one receiver without PS");
    p.pureSignal = true;
    check(MetisClient::effectiveNumRx(p) == 4, "PS streams four DDCs");
    p.boardMaxRx = 0;
    check(MetisClient::effectiveNumRx(p) == 4, "unknown board: four");
    p.boardMaxRx = 2;
    check(MetisClient::effectiveNumRx(p) == 1,
          "a two-DDC board gets no floor (the backend refuses PS there)");
    p.boardMaxRx = 8;
    p.numRx = 6;
    check(MetisClient::effectiveNumRx(p) == 6, "more than four is left alone");
}

void statusFields()
{
    hl2::Hl2PureSignalStatus s;
    s.available = true;
    s.enabled = true;
    s.correcting = true;
    s.feedbackLevel = 150;
    s.calibrationCount = 3;
    s.engineState = Hl2PsController::LStayOn;
    s.attenuationDb = 12;
    s.feedbackLimit = 1;
    s.keyed = true;
    const QVariantMap f = hl2::pureSignalStatusFields(s);
    check(f.value(QStringLiteral("available")).toBool()
          && f.value(QStringLiteral("enabled")).toBool()
          && f.value(QStringLiteral("correcting")).toBool()
          && f.value(QStringLiteral("keyed")).toBool(), "flags reach the extension map");
    check(f.value(QStringLiteral("feedbackLevel")).toInt() == 150
          && f.value(QStringLiteral("calibrationCount")).toInt() == 3
          && f.value(QStringLiteral("engineState")).toInt() == 8
          && f.value(QStringLiteral("attenuationDb")).toInt() == 12
          && f.value(QStringLiteral("feedbackLimit")).toInt() == 1, "values reach the map");
    check(f.value(QStringLiteral("attMin")).toInt() == -28
          && f.value(QStringLiteral("attMax")).toInt() == 31, "the map carries the att range");
    check(f.value(QStringLiteral("feedbackWindowLow")).toInt() == 128
          && f.value(QStringLiteral("feedbackWindowHigh")).toInt() == 181,
          "the map carries calcc's window");
    check(f.contains(QStringLiteral("unavailableReason"))
          && f.contains(QStringLiteral("autoCalibrate"))
          && f.contains(QStringLiteral("autoAttenuate"))
          && f.contains(QStringLiteral("calibrating")), "every key is always present");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    gainStep();
    limitNeedsALevelMeasuredHere();
    autoOnAndOff();
    singleCalibration();
    singleGivesUp();
    restoring();
    autoAttenuate();
    singleGainMovesBounded();
    controllerAutoSwitch();
    receiverCountUnderFloor();
    wire();
    statusFields();
    if (g_failures == 0)
        std::puts("hl2_puresignal_test: all checks passed");
    return g_failures == 0 ? 0 : 1;
}
