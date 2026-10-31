// Two Tone TUNE refuses on a radio that declares no two-tone generator
// (RadioCapabilities::twoToneGenerator empty, as on the Hermes-Lite 2), on
// every GUI road, instead of keying a single carrier.
//
// Pinned, socket-free, no RF:
//   * the TUNE right-click menu's "Two Tone" entry is disabled with its reason
//     on an HL2-like capability set, enabled with a generator, enabled with no
//     radio connected;
//   * the MIDI action (dispatchMidiTxInput, the function MainWindow calls)
//     is consumed, reports the reason and issues no keying call on HL2-like
//     caps, and reaches the tune call with a generator;
//   * the predicate the keyboard shortcut calls (twoToneTuneMayStart) gives the
//     same answers.
// NOT covered: the shortcut lambda in MainWindow::registerShortcutActions and
// the status-bar display, which need a full MainWindow; each is one call to
// twoToneTuneMayStart.

#include "TestSettingsProfile.h"
#include "core/backends/RadioCapabilities.h"
#include "gui/MidiTxDispatch.h"
#include "gui/TwoToneTuneGate.h"
#include "gui/TxApplet.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QAction>
#include <QApplication>
#include <QMenu>

#include <cstdio>
#include <memory>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

// The production backend seam, no sockets. Capabilities are chosen per test.
class GateBackend final : public IRadioBackend {
public:
    QStringList commands;
    bool connected{false};
    bool withGenerator{false};
    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps;
        caps.canTransmit = true;
        caps.hasTuner = true;
        if (withGenerator) {
            caps.twoToneGenerator = RadioCapabilities::TwoToneGenerator{QStringLiteral("test")};
        } else {
            caps.twoToneGenerator = std::nullopt;
        }
        return caps;
    }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool on, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override
    { commands << (on ? "mox:on" : "mox:off"); }
    void setAtu(bool on, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override
    { commands << (on ? "atu:on" : "atu:off"); }
    void setTune(bool on, int, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override
    { commands << (on ? "tune:on" : "tune:off"); }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

struct Fixture {
    RadioModel radio;
    GateBackend* backend{nullptr};
    Fixture(bool withGenerator, bool connected)
    {
        auto b = std::make_unique<GateBackend>();
        backend = b.get();
        backend->withGenerator = withGenerator;
        radio.setBackendForTest(std::move(b), QStringLiteral("test"));
        check(radio.automationApplySliceFixture(0, QStringLiteral("A")), "slice fixture");
        SliceDelta slice;
        slice.txSlice = true;
        slice.mode = QStringLiteral("USB");
        slice.panId = QStringLiteral("0x40000000");
        radio.slice(0)->applyChanges(slice);
        backend->connected = connected;
        radio.transmitModel().setTxModeGetter([] { return QStringLiteral("USB"); });
        radio.transmitModel().setHasTuner(true);
    }
};

static QAction* twoToneAction(Fixture& f, QMenu& menu)
{
    TxApplet applet;
    applet.setRadioModel(&f.radio);
    applet.setTransmitModel(&f.radio.transmitModel());
    applet.buildTuneContextMenu(menu);
    for (QAction* a : menu.actions()) {
        if (a->text() == QLatin1String("Two Tone")) return a;
    }
    return nullptr;
}

static void menuCases()
{
    {
        Fixture f(/*withGenerator*/ false, /*connected*/ true);
        QMenu menu;
        QAction* two = twoToneAction(f, menu);
        check(two != nullptr, "menu has a Two Tone entry");
        if (two) {
            check(!two->isEnabled(), "HL2-like: Two Tone entry is disabled");
            check(two->statusTip().startsWith(QLatin1String("Unavailable:"))
                      && two->toolTip() == two->statusTip(),
                  "HL2-like: reason is in statusTip and toolTip");
        }
    }
    {
        Fixture f(true, true);
        QMenu menu;
        QAction* two = twoToneAction(f, menu);
        check(two && two->isEnabled() && two->statusTip().isEmpty(),
              "generator declared: Two Tone entry is enabled, no reason");
    }
    {
        Fixture f(false, /*connected*/ false);
        QMenu menu;
        QAction* two = twoToneAction(f, menu);
        check(two && two->isEnabled(), "no radio connected: Two Tone entry is enabled");
    }
}

static void predicateCases()
{
    RadioCapabilities hl2;
    hl2.twoToneGenerator = std::nullopt;
    RadioCapabilities flex;
    flex.twoToneGenerator = RadioCapabilities::TwoToneGenerator{QStringLiteral("test")};
    check(!twoToneTuneAvailable(true, hl2), "predicate: connected HL2-like is refused");
    check(twoToneTuneAvailable(true, flex), "predicate: connected with generator is allowed");
    check(twoToneTuneAvailable(false, hl2), "predicate: not connected is allowed");

    for (const bool withGenerator : {false, true}) {
        Fixture f(withGenerator, true);
        int shown = 0;
        QString reason;
        const bool ok = twoToneTuneMayStart(f.radio, [&](const QString& why) { ++shown; reason = why; });
        check(ok == withGenerator, "shortcut predicate follows the capability");
        check(withGenerator ? shown == 0 : (shown == 1 && reason.startsWith(QLatin1String("Unavailable:"))),
              "shortcut predicate reports the reason only when it refuses");
    }
    Fixture off(false, false);
    check(twoToneTuneMayStart(off.radio, {}), "shortcut predicate: not connected is allowed");
}

static void midiCases()
{
    const auto cw = [](const QString&, bool, const std::shared_ptr<TxController>&) {};
    for (const bool withGenerator : {false, true}) {
        Fixture f(withGenerator, true);
        QObject device;
        const auto controller = TxController::forNativeDevice(&f.radio, &device);
        const auto captured = TxController::captureInputScope(controller, controller->captureRawInput());
        int shown = 0;
        const bool consumed = dispatchMidiTxInput("global.twoToneTune", 1.0f, f.radio, captured, cw,
                                                  [&](const QString&) { ++shown; });
        if (withGenerator) {
            check(consumed && shown == 0 && f.backend->commands == QStringList{"tune:on"}
                      && f.radio.transmitModel().isTuning(),
                  "generator declared: MIDI Two Tone reaches the tune call");
        } else {
            check(consumed && shown == 1 && f.backend->commands.isEmpty()
                      && !f.radio.transmitModel().isTuning(),
                  "HL2-like: MIDI Two Tone is consumed, reports, and makes no keying call");
        }
    }
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("two-tone-tune-gate-test"));
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);

    menuCases();
    predicateCases();
    midiCases();

    std::printf("two_tone_tune_gate_test: %s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
