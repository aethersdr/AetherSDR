// HL2 PureSignal (MI0BOT's Thetis design): the control state machines, the
// auto-attenuate step, and the wire facts they lean on — the 0x0a PS bit, the
// keyed feedback gain and the four-DDC floor. Clock-free and socket-free: the
// logic is driven tick by tick with the GetPSInfo values a radio would give.

#include "core/backends/hl2/Hl2PureSignal.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <cstdio>

using AetherSDR::hl2::Hl2PureSignalLogic;
using AetherSDR::hl2::MetisClient;
namespace hl2 = AetherSDR::hl2;

namespace {

int g_failures = 0;
void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

using Ctl = Hl2PureSignalLogic::Control;
using Cmd = Hl2PureSignalLogic::Cmd;
using Info = Hl2PureSignalLogic::Info;

constexpr Ctl kReset{true, false, false, false};

void attenuationStep()
{
    check(Hl2PureSignalLogic::attenuationStepDb(152) == 0, "152 is on target");
    check(Hl2PureSignalLogic::attenuationStepDb(0) == -10, "no feedback steps -10");
    check(Hl2PureSignalLogic::attenuationStepDb(300) == 10, "beyond 256 steps +10");
    check(Hl2PureSignalLogic::attenuationStepDb(76) == -6, "half the target is -6 dB");
    check(Hl2PureSignalLogic::attenuationStepDb(256) == 5, "256 rounds 4.51 to 5");
    check(Hl2PureSignalLogic::attenuationStepDb(30) == -14, "30 is -14 dB");

    check(Hl2PureSignalLogic::needToRecalibrate(182, 0), "too hot recalibrates");
    check(!Hl2PureSignalLogic::needToRecalibrate(150, 0), "in window holds");
    check(Hl2PureSignalLogic::needToRecalibrate(100, 0), "too cold recalibrates");
    check(!Hl2PureSignalLogic::needToRecalibrate(100, Hl2PureSignalLogic::kAttMin),
          "too cold at minimum attenuation has nowhere to go");
    check(Hl2PureSignalLogic::feedbackInRange(129) && !Hl2PureSignalLogic::feedbackInRange(128)
          && Hl2PureSignalLogic::feedbackInRange(181) && !Hl2PureSignalLogic::feedbackInRange(182),
          "window is 128 < fb <= 181");
    check(Hl2PureSignalLogic::clampAtt(-40) == -28 && Hl2PureSignalLogic::clampAtt(40) == 31,
          "attenuation clamps to -28..31");
    check(Hl2PureSignalLogic::lnaGainDbForAtt(31) == -12
          && Hl2PureSignalLogic::lnaGainDbForAtt(-28) == 47, "gain is 19 - att");
}

void autoCalibrateOnAndOff()
{
    Hl2PureSignalLogic l;
    l.setAutoAttenuate(false);
    l.setAutoCalibrate(true);
    auto o = l.tick({});
    check(o.controls.size() == 1 && o.controls[0] == kReset, "OFF sends reset");
    check(l.command() == Cmd::TurnOnAuto, "OFF -> TurnOnAuto");
    o = l.tick({});
    check(o.controls.size() == 1 && o.controls[0] == Ctl{true, false, true, false},
          "TurnOnAuto sends automode");
    check(l.command() == Cmd::Auto && l.calibrating(), "-> Auto");
    o = l.tick({});
    check(o.controls.empty() && l.command() == Cmd::Auto, "Auto idles");

    l.setAutoCalibrate(false);
    l.tick({});
    check(l.command() == Cmd::TurnOff, "Auto off -> TurnOff");
    Info stillOn;
    stillOn.correcting = true;
    stillOn.state = Hl2PureSignalLogic::LStayOn;
    o = l.tick(stillOn);
    check(o.controls.size() == 1 && o.controls[0] == kReset, "TurnOff resets");
    check(l.command() == Cmd::TurnOff, "TurnOff waits for the engine to reset");
    l.tick({});
    check(l.command() == Cmd::Off && !l.calibrating(), "engine reset -> OFF");
}

void singleCalibration()
{
    Hl2PureSignalLogic l;
    l.setAutoAttenuate(false);
    l.requestSingleCalibration();
    l.tick({});
    check(l.command() == Cmd::TurnOnSingle, "OFF -> TurnOnSingle");
    auto o = l.tick({});
    check(o.controls.size() == 1 && o.controls[0] == Ctl{true, true, false, false},
          "TurnOnSingle sends mancal");
    check(l.command() == Cmd::Single, "-> Single");
    l.tick({});
    check(l.command() == Cmd::Single, "Single waits for a correction");
    Info done;
    done.correcting = true;
    done.feedbackLevel = 150;
    l.tick(done);
    check(l.command() == Cmd::StayOn && !l.calibrating(), "correction applied -> StayOn");
    o = l.tick(done);
    check(o.controls.empty() && l.command() == Cmd::StayOn, "StayOn holds the correction");
}

void restore()
{
    Hl2PureSignalLogic l;
    l.setAutoAttenuate(false);
    l.requestRestore();
    l.tick({});
    check(l.command() == Cmd::Restore, "OFF -> Restore");
    auto o = l.tick({});
    check(o.controls.size() == 1 && o.controls[0] == Ctl{false, false, false, true},
          "Restore sends turnon");
    Info on;
    on.state = Hl2PureSignalLogic::LStayOn;
    on.correcting = true;
    l.tick(on);
    check(l.command() == Cmd::StayOn, "restored correction -> StayOn");
}

void autoAttenuate()
{
    Hl2PureSignalLogic l;
    l.setAutoCalibrate(true);
    check(l.attenuationDb() == 31, "starts at the lowest gain");
    l.tick({});
    l.tick({});
    check(l.command() == Cmd::Auto, "auto running");

    // A calibration attempt with the feedback far too low.
    Info cold;
    cold.feedbackLevel = 30;
    cold.calibrationCount = 1;
    auto o = l.tick(cold);
    check(l.autoAttenuateState() == Hl2PureSignalLogic::Aa::SetNewValues, "-> SetNewValues");
    check(o.controls.size() == 1 && o.controls[0] == kReset, "interrupts with a reset");
    check(!o.attenuationDb, "attenuation moves on the next tick");
    o = l.tick(cold);
    check(o.attenuationDb && *o.attenuationDb == 17, "31 - 14 = 17 dB");
    check(l.attenuationDb() == 17, "logic holds 17");
    o = l.tick(cold);
    check(o.controls.size() == 1 && o.controls[0] == Ctl{false, false, true, false},
          "resumes auto");
    check(l.autoAttenuateState() == Hl2PureSignalLogic::Aa::Monitor, "-> Monitor");
    // Same attempt count: nothing new to judge.
    o = l.tick(cold);
    check(o.controls.empty(), "waits for the next attempt");

    // In the window: no change.
    Info good;
    good.feedbackLevel = 150;
    good.calibrationCount = 2;
    o = l.tick(good);
    check(o.controls.empty() && l.autoAttenuateState() == Hl2PureSignalLogic::Aa::Monitor,
          "in window holds");

    // Off: never touches attenuation.
    Hl2PureSignalLogic manual;
    manual.setAutoAttenuate(false);
    manual.setAutoCalibrate(true);
    manual.tick({});
    manual.tick({});
    o = manual.tick(cold);
    check(o.controls.empty() && !o.attenuationDb && manual.attenuationDb() == 31,
          "auto-attenuate off leaves attenuation alone");
}

void wire()
{
    const hl2::Cc plain = hl2::ccGain(10, false);
    const hl2::Cc ps = hl2::ccGain(10, true);
    check(plain == hl2::ccRxGain(10), "PS off is the plain gain bank");
    check((ps[2] & hl2::kGainPureSignal) != 0 && (plain[2] & hl2::kGainPureSignal) == 0,
          "PS sets 0x0a C2 bit 6");
    check(ps[4] == plain[4], "PS bit leaves the gain byte");
    check(hl2::ccGain(Hl2PureSignalLogic::lnaGainDbForAtt(31), true)[4] == 0x40,
          "att 31 is ((31 - 31) & 0x3F) | 0x40");
    check(hl2::ccGain(Hl2PureSignalLogic::lnaGainDbForAtt(0), true)[4] == (0x40 | 31),
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
    s.engineState = Hl2PureSignalLogic::LStayOn;
    s.attenuationDb = 12;
    s.keyed = true;
    const QVariantMap f = hl2::pureSignalStatusFields(s);
    check(f.value(QStringLiteral("available")).toBool()
          && f.value(QStringLiteral("enabled")).toBool()
          && f.value(QStringLiteral("correcting")).toBool()
          && f.value(QStringLiteral("keyed")).toBool(), "flags reach the extension map");
    check(f.value(QStringLiteral("feedbackLevel")).toInt() == 150
          && f.value(QStringLiteral("calibrationCount")).toInt() == 3
          && f.value(QStringLiteral("engineState")).toInt() == 8
          && f.value(QStringLiteral("attenuationDb")).toInt() == 12, "values reach the map");
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

int main()
{
    attenuationStep();
    autoCalibrateOnAndOff();
    singleCalibration();
    restore();
    autoAttenuate();
    wire();
    statusFields();
    if (g_failures == 0)
        std::puts("hl2_puresignal_test: all checks passed");
    return g_failures == 0 ? 0 : 1;
}
