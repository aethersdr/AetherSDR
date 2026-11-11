// A serial CW key or paddle line already active when the port opens keys nothing.
//
// The PTT pins have always been baselined from the open() sample; the key and
// paddle lines now are too, so a line that reads active at open needs a release
// and a press before it keys. Injected samples through the production sampling
// path (the poll and the Win32 watcher share it); no port is opened, so open()'s
// own call to the baseline is source, not exercised here. The same holds for
// loadSettings()'s in-place reload: its rebaseline runs here, the call does not.
// The Win32 watcher's epoch filter is not run either; its input, the epoch, is.

#include "TestSettingsProfile.h"
#include "core/SerialPortController.h"

#include <QCoreApplication>
#include <QObject>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace AetherSDR {
class SerialInputBaselineTestAccess {
public:
    static void open(SerialPortController& c, bool cts, bool dsr, bool dcd)
    {
        c.baselineInputs(cts, dsr, dcd);
    }
    static void sample(SerialPortController& c, bool cts, bool dsr, bool dcd)
    {
        c.applyInputSample(cts, dsr, dcd, {});
    }
    static void reload(SerialPortController& c, bool cts, bool dsr, bool dcd)
    {
        c.rebaselineInPlace(cts, dsr, dcd);
    }
    static quint64 portEpoch(const SerialPortController& c)
    {
        return c.m_inputPortEpoch.load();
    }
    static void close(SerialPortController& c) { c.retireTxInputs(); }
};
} // namespace AetherSDR

using AetherSDR::SerialInputBaselineTestAccess;
using AetherSDR::SerialPortController;
using Input = SerialPortController::InputFunction;
using Request = AetherSDR::TxCoordinator::Request;

namespace {

int failures = 0;

void check(bool condition, const std::string& what)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!condition) {
        ++failures;
    }
}

struct Recorder {
    std::vector<bool> keys;
    std::vector<std::pair<bool, bool>> paddles;
    std::vector<bool> ptts;
    explicit Recorder(SerialPortController& c)
    {
        QObject::connect(&c, &SerialPortController::cwKeyChanged, &c,
                         [this](bool down, const Request&) { keys.push_back(down); });
        QObject::connect(&c, &SerialPortController::cwPaddleChanged, &c,
                         [this](bool dit, bool dah, const Request&, const Request&) {
            paddles.emplace_back(dit, dah);
        });
        QObject::connect(&c, &SerialPortController::externalPttChanged, &c,
                         [this](bool active, const Request&) { ptts.push_back(active); });
    }
    bool anyKeyDown() const
    {
        for (bool k : keys) {
            if (k) {
                return true;
            }
        }
        return false;
    }
    bool anyPaddleDown() const
    {
        for (const auto& [dit, dah] : paddles) {
            if (dit || dah) {
                return true;
            }
        }
        return false;
    }
};

// Straight key on CTS. `activeHigh` false means the key reads active on a low line.
void straightKey(bool activeHigh)
{
    const std::string tag = activeHigh ? "key (active high)" : "key (active low)";
    const bool pressed = activeHigh;
    SerialPortController c;
    c.setCtsFunction(Input::CwKeyInput);
    c.setCtsPolarity(activeHigh);
    Recorder r(c);

    SerialInputBaselineTestAccess::open(c, pressed, false, false);
    SerialInputBaselineTestAccess::sample(c, pressed, false, false);
    SerialInputBaselineTestAccess::sample(c, pressed, false, false);
    check(!r.anyKeyDown(), tag + ": a line active at open emits no key-down at the first polls");

    SerialInputBaselineTestAccess::sample(c, !pressed, false, false);
    check(r.keys.empty(), tag + ": its release emits nothing, no unpaired key-up");
    SerialInputBaselineTestAccess::sample(c, pressed, false, false);
    check(r.anyKeyDown() && r.keys.back(), tag + ": the next press keys");
}

void idleKeyControl()
{
    SerialPortController c;
    c.setCtsFunction(Input::CwKeyInput);
    c.setCtsPolarity(true);
    Recorder r(c);
    SerialInputBaselineTestAccess::open(c, false, false, false);
    SerialInputBaselineTestAccess::sample(c, true, false, false);
    check(r.keys.size() == 1 && r.keys.front(),
          "control: a key idle at open keys on the first press");
}

// Dit on DSR, dah on DCD, optionally swapped in software.
void paddle(bool swap)
{
    const std::string tag = swap ? "paddle (swapped)" : "paddle";
    SerialPortController c;
    c.setDsrFunction(Input::CwDitInput);
    c.setDcdFunction(Input::CwDahInput);
    c.setDsrPolarity(true);
    c.setDcdPolarity(true);
    c.setPaddleSwap(swap);
    Recorder r(c);

    SerialInputBaselineTestAccess::open(c, false, true, false);
    SerialInputBaselineTestAccess::sample(c, false, true, false);
    check(!r.anyPaddleDown(), tag + ": a paddle line active at open emits no element");

    SerialInputBaselineTestAccess::sample(c, false, false, false);
    check(r.paddles.empty(), tag + ": its release emits nothing, no unpaired release");
    SerialInputBaselineTestAccess::sample(c, false, true, false);
    const bool wantDit = !swap;
    check(!r.paddles.empty() && r.paddles.back().first == wantDit
              && r.paddles.back().second == !wantDit,
          tag + ": the next press sends the element its role and swap name");
}

void pttUnchanged()
{
    SerialPortController c;
    c.setCtsFunction(Input::PttInput);
    c.setCtsPolarity(true);
    Recorder r(c);
    SerialInputBaselineTestAccess::open(c, true, false, false);
    SerialInputBaselineTestAccess::sample(c, true, false, false);
    check(r.ptts.empty(), "a PTT line active at open still asserts no PTT");
}

// Polarity flipped with the port open (loadSettings() in place) on an idle key.
void polarityFlipIdleKey()
{
    const std::string tag = "polarity flip, key idle";
    SerialPortController c;
    c.setCtsFunction(Input::CwKeyInput);
    c.setCtsPolarity(true);
    Recorder r(c);
    SerialInputBaselineTestAccess::open(c, false, false, false);
    SerialInputBaselineTestAccess::sample(c, false, false, false);

    c.setCtsPolarity(false);  // the idle-low line now reads active
    SerialInputBaselineTestAccess::reload(c, false, false, false);
    SerialInputBaselineTestAccess::sample(c, false, false, false);
    check(!r.anyKeyDown(), tag + ": the next poll emits no key-down");
    SerialInputBaselineTestAccess::sample(c, true, false, false);
    SerialInputBaselineTestAccess::sample(c, false, false, false);
    check(r.anyKeyDown() && r.keys.back(), tag + ": a release and a press key");
}

// Polarity flipped while the key is held: the hold is released, not dropped.
void polarityFlipHeldKey()
{
    const std::string tag = "polarity flip, key held";
    SerialPortController c;
    c.setCtsFunction(Input::CwKeyInput);
    c.setCtsPolarity(true);
    Recorder r(c);
    SerialInputBaselineTestAccess::open(c, false, false, false);
    SerialInputBaselineTestAccess::sample(c, true, false, false);
    check(r.keys.size() == 1 && r.keys.front(), tag + ": control: the press keys");

    c.setCtsPolarity(false);
    const quint64 epoch = SerialInputBaselineTestAccess::portEpoch(c);
    SerialInputBaselineTestAccess::reload(c, true, false, false);
    SerialInputBaselineTestAccess::sample(c, true, false, false);
    check(r.keys.size() == 2 && !r.keys.back(), tag + ": the reload publishes the key-up");
    // The Win32 watcher captured the epoch at start and drops samples from
    // any other one: the port stays open, so the reload must keep it.
    check(SerialInputBaselineTestAccess::portEpoch(c) == epoch,
          tag + ": the in-place reload keeps the port epoch");
}

// A held paddle is released by the reload too, with the epoch kept.
void reloadHeldPaddle()
{
    const std::string tag = "reload, paddle held";
    SerialPortController c;
    c.setDsrFunction(Input::CwDitInput);
    c.setDsrPolarity(true);
    Recorder r(c);
    SerialInputBaselineTestAccess::open(c, false, false, false);
    SerialInputBaselineTestAccess::sample(c, false, true, false);
    check(r.paddles.size() == 1 && r.paddles.front().first, tag + ": control: the press sends dit");
    const quint64 epoch = SerialInputBaselineTestAccess::portEpoch(c);
    SerialInputBaselineTestAccess::reload(c, false, true, false);
    check(r.paddles.size() == 2 && !r.paddles.back().first && !r.paddles.back().second,
          tag + ": the reload publishes the release");
    check(SerialInputBaselineTestAccess::portEpoch(c) == epoch,
          tag + ": the in-place reload keeps the port epoch");
}

// Control for the epoch check: closing the port does move it.
void closeMovesEpoch()
{
    SerialPortController c;
    const quint64 epoch = SerialInputBaselineTestAccess::portEpoch(c);
    SerialInputBaselineTestAccess::close(c);
    check(SerialInputBaselineTestAccess::portEpoch(c) != epoch,
          "control: closing the port moves the port epoch");
}

void polarityFlipIdlePaddle()
{
    SerialPortController c;
    c.setDsrFunction(Input::CwDitInput);
    c.setDsrPolarity(true);
    Recorder r(c);
    SerialInputBaselineTestAccess::open(c, false, false, false);
    c.setDsrPolarity(false);
    SerialInputBaselineTestAccess::reload(c, false, false, false);
    SerialInputBaselineTestAccess::sample(c, false, false, false);
    check(!r.anyPaddleDown(), "polarity flip, paddle idle: the next poll emits no element");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("serial-cw-key-baseline"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "Cannot create isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);

    idleKeyControl();
    straightKey(true);
    straightKey(false);
    paddle(false);
    paddle(true);
    pttUnchanged();
    polarityFlipIdleKey();
    polarityFlipHeldKey();
    polarityFlipIdlePaddle();
    reloadHeldPaddle();
    closeMovesEpoch();

    if (failures == 0) {
        std::printf("serial_cw_key_baseline_test: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
