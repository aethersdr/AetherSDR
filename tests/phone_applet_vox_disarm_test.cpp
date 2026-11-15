// The automation bridge may switch VOX off, with or without TX permission, and never on.
// The Phone applet's VOX button carries a disarm-only scoped action (#6053).
// Production shortcut registration/dispatch is exercised by tools/automation_vox_guard.py.
// Every other path is pinned: arming and pointer verbs are refused, the operator's own input is untouched.
// No radio, no socket, no transmission: a bare RadioModel's TransmitModel.

#include "TestEventLoop.h"
#include "TestSettingsProfile.h"
#include "core/AutomationServer.h"
#include "core/ShortcutManager.h"
#include "core/TxKeyingMarker.h"
#include "gui/PhoneApplet.h"
#include "gui/TxKeyActivationGuard.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMainWindow>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTest>

#include <atomic>
#include <cstdio>

namespace AetherSDR {
class AutomationServerTestAccess {
public:
    static QJsonObject request(AutomationServer& server, const QJsonObject& object)
    {
        return server.handleLine(QJsonDocument(object).toJson(QJsonDocument::Compact), nullptr);
    }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {

int failures = 0;
std::atomic<int> nameMatchWarnings{0};
QtMessageHandler previousHandler = nullptr;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    failures += ok ? 0 : 1;
}

void captureNameMatch(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (msg.contains(QStringLiteral("TX guard fell back to name match on VOX"))) {
        ++nameMatchWarnings;
    }
    if (previousHandler) {
        previousHandler(type, ctx, msg);
    }
}

const QString kVox = QStringLiteral("VOX voice-operated transmit");

QJsonObject invoke(AutomationServer& server, const QString& action, const QString& value = {})
{
    QJsonObject o{{QStringLiteral("cmd"), QStringLiteral("invoke")},
                  {QStringLiteral("target"), kVox},
                  {QStringLiteral("action"), action}};
    if (!value.isEmpty()) {
        o[QStringLiteral("value")] = value;
    }
    return AutomationServerTestAccess::request(server, o);
}

QJsonObject clickAt(AutomationServer& server)
{
    return AutomationServerTestAccess::request(server,
        QJsonObject{{QStringLiteral("cmd"), QStringLiteral("clickAt")},
                    {QStringLiteral("target"), kVox},
                    {QStringLiteral("value"), QStringLiteral("5 5")}});
}

bool ok(const QJsonObject& reply) { return reply.value(QStringLiteral("ok")).toBool(); }

// The bridge defers an accepted invoke; a refused one must never land later.
void drain() { AetherTest::pumpFor(100); }

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-phone-applet-vox-disarm-test"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "FAIL could not create isolated settings profile\n");
        return 1;
    }
    qunsetenv("AETHER_AUTOMATION_ALLOW_TX");
    previousHandler = qInstallMessageHandler(captureNameMatch);
    QApplication app(argc, argv);

    RadioModel radio;
    TransmitModel& tx = radio.transmitModel();
    PhoneApplet applet;
    applet.setTransmitModel(&tx);
    applet.show();
    AetherTest::waitFor([&] { return applet.isVisible(); }, 2000);

    QPushButton* button = nullptr;
    for (auto* b : applet.findChildren<QPushButton*>()) {
        if (b->accessibleName() == kVox) {
            button = b;
        }
    }
    check(button != nullptr, "the Phone applet has the VOX button");
    if (!button) {
        return 1;
    }

    // ── Classification ──────────────────────────────────────────────────────
    check(transmitControlMatch(button) == TransmitControlMatch::Marker,
          "the VOX button is a TX control by marker, not by a word in its name");
    check(txActionRequiresPermission(button),
          "the VOX button requires bridge TX permission");

    AutomationServer server;
    server.setRadioModel(&radio);

    // ── Bridge, TX permission granted ───────────────────────────────────────
    server.setTxAllowed(true);
    tx.setVoxEnable(true);
    check(tx.voxEnable() && button->isChecked(), "precondition: VOX armed, button shows it");
    nameMatchWarnings = 0;
    check(ok(invoke(server, QStringLiteral("setChecked"), QStringLiteral("false"))),
          "TX allowed: invoke setChecked false is accepted");
    check(AetherTest::waitFor([&] { return !tx.voxEnable() && !button->isChecked(); }, 2000),
          "TX allowed: the disarm reaches TransmitModel and the button follows");
    check(nameMatchWarnings == 0, "TX allowed: no name-fallback warning for VOX");

    // "unchecked" is what the bridge reads back for an unchecked button.
    for (const char* spelling : {"0", "off", "no", " FALSE ", "unchecked"}) {
        tx.setVoxEnable(true);
        const bool accepted = ok(invoke(server, QStringLiteral("setChecked"),
                                        QString::fromLatin1(spelling)));
        const bool landed = AetherTest::waitFor([&] { return !tx.voxEnable(); }, 2000);
        const QByteArray what = QByteArray("TX allowed: setChecked '") + spelling
                                + "' disarms";
        check(accepted && landed, what.constData());
    }

    // The model is the subject: armed model, button reading off.
    tx.setVoxEnable(true);
    {
        const QSignalBlocker block(button);
        button->setChecked(false);
    }
    check(tx.voxEnable() && !button->isChecked(), "precondition: model armed, button reads off");
    check(ok(invoke(server, QStringLiteral("setChecked"), QStringLiteral("false")))
              && AetherTest::waitFor([&] { return !tx.voxEnable(); }, 2000),
          "TX allowed: a disarm switches the model off even when the button disagrees");

    check(ok(invoke(server, QStringLiteral("setChecked"), QStringLiteral("false"))),
          "TX allowed: disarming an already-off VOX is accepted");
    drain();
    check(!tx.voxEnable(), "TX allowed: and VOX stays off");

    // Arming, VOX off: every verb refused.
    {
        const bool on = ok(invoke(server, QStringLiteral("setChecked"), QStringLiteral("true")));
        const bool one = ok(invoke(server, QStringLiteral("setChecked"), QStringLiteral("1")));
        const bool click = ok(invoke(server, QStringLiteral("click")));
        const bool toggle = ok(invoke(server, QStringLiteral("toggle")));
        const bool pointer = ok(clickAt(server));
        drain();
        check(!on && !one, "TX allowed: setChecked true is refused (unchanged)");
        check(!click && !toggle, "TX allowed: click and toggle are refused (unchanged)");
        check(!pointer, "TX allowed: clickAt on the button is refused (unchanged)");
        check(!tx.voxEnable() && !button->isChecked(),
              "TX allowed: nothing armed VOX behind a refusal");
    }

    // Toggling verbs, VOX on: still refused, they are not a disarm.
    {
        tx.setVoxEnable(true);
        const bool click = ok(invoke(server, QStringLiteral("click")));
        const bool toggle = ok(invoke(server, QStringLiteral("toggle")));
        const bool pointer = ok(clickAt(server));
        drain();
        check(!click && !toggle && !pointer && tx.voxEnable(),
              "TX allowed: click, toggle and clickAt on an armed VOX are refused (unchanged)");
        tx.setVoxEnable(false);
    }

    // ── Bridge, no TX permission: disarm passes, arming is refused ──────────
    server.setTxAllowed(false);
    {
        tx.setVoxEnable(true);
        const bool off = ok(invoke(server, QStringLiteral("setChecked"), QStringLiteral("false")));
        const bool landed = AetherTest::waitFor([&] { return !tx.voxEnable() && !button->isChecked(); }, 2000);
        check(off && landed, "TX not allowed: invoke setChecked false is accepted and disarms VOX (Principle VI)");
        check(!tx.voxEnable() && !button->isChecked(), "TX not allowed: VOX is disarmed");

        for (const char* spelling : {"0", "off", "no", "unchecked"}) {
            tx.setVoxEnable(true);
            const bool accepted = ok(invoke(server, QStringLiteral("setChecked"),
                                            QString::fromLatin1(spelling)));
            const bool disarmed = AetherTest::waitFor([&] { return !tx.voxEnable(); }, 2000);
            const QByteArray what = QByteArray("TX not allowed: setChecked '") + spelling + "' disarms";
            check(accepted && disarmed, what.constData());
        }

        const bool click = ok(invoke(server, QStringLiteral("click")));
        const bool pointer = ok(clickAt(server));
        drain();
        check(!click && !pointer, "TX not allowed: click and clickAt are refused (unchanged)");

        tx.setVoxEnable(false);
        const bool on = ok(invoke(server, QStringLiteral("setChecked"), QStringLiteral("true")));
        const bool toggle = ok(invoke(server, QStringLiteral("toggle")));
        const bool pointerOn = ok(clickAt(server));
        drain();
        check(!on && !toggle && !pointerOn && !tx.voxEnable(),
              "TX not allowed: arming is refused by every verb (unchanged)");
    }

    // The injected result tests the bridge's response mapping only. The full
    // app probe exercises MainWindow's registration and dispatch independently.
    {
        QMainWindow window;
        window.show();
        bool dispatched = false;
        bool permission = true;
        server.setShortcutAutomationHandler([&](const QString& id, bool allowTx,
                                                const std::shared_ptr<TxController>&) {
            dispatched = id == QStringLiteral("vox_toggle");
            permission = allowTx;
            return 3; // MainWindow::ShortcutFireTxBlocked
        });
        server.setTxAllowed(false);
        const QJsonObject rep = AutomationServerTestAccess::request(server,
            QJsonObject{{QStringLiteral("cmd"), QStringLiteral("shortcut")},
                        {QStringLiteral("target"), QStringLiteral("vox_toggle")}});
        check(dispatched && !permission, "shortcut: bridge dispatches with TX permission off");
        check(!ok(rep) && rep.value(QStringLiteral("error")).toString().startsWith(
                  QStringLiteral("blocked: 'vox_toggle' keys the transmitter (TX-safety guard).")),
              "shortcut: TX-blocked result produces the specific safety refusal");
        server.setShortcutAutomationHandler({});
    }

    // ── The operator's own input ────────────────────────────────────────────
    button->setChecked(true);
    check(tx.voxEnable(), "operator: the button's toggled signal arms the model");
    button->setChecked(false);
    check(!tx.voxEnable(), "operator: the button's toggled signal disarms the model");

    QTest::mouseClick(button, Qt::LeftButton);
    check(tx.voxEnable() && button->isChecked(), "operator: a mouse click arms VOX");
    QTest::mouseClick(button, Qt::LeftButton);
    check(!tx.voxEnable() && !button->isChecked(), "operator: a mouse click disarms VOX");

    QTest::keyClick(button, Qt::Key_Space);
    check(tx.voxEnable(), "operator: Space on the button arms VOX when nothing filters it");
    QTest::keyClick(button, Qt::Key_Space);
    check(!tx.voxEnable(), "operator: Space on the button disarms VOX when nothing filters it");

    // ── Keyboard TX activation guard, as MainWindow consults it ─────────────
    {
        ShortcutManager keys;
        keys.registerAction(QStringLiteral("ptt_hold"), QStringLiteral("PTT (Hold)"),
                            QStringLiteral("TX"), QKeySequence(Qt::Key_Space), nullptr, false, true);
        keys.registerAction(QStringLiteral("enter_marker"), QStringLiteral("Enter marker"),
                            QStringLiteral("Display"), QKeySequence(Qt::Key_Enter), nullptr);
        const QKeyEvent spacePress(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
        const QKeyEvent spaceRelease(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
        const QKeyEvent enterPress(QEvent::KeyPress, Qt::Key_Enter, Qt::NoModifier);
        const QKeyEvent returnPress(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);

        check(isTxKeyingButton(button),
              "guard: the VOX button counts as a TX-keying button (unchanged)");
        check(refuseTxKeyActivation(button, &spacePress, false, keys)
                  && refuseTxKeyActivation(button, &spaceRelease, false, keys),
              "guard, shortcuts off: bound Space on the VOX button is refused (unchanged)");
        check(refuseTxKeyActivation(button, &enterPress, false, keys),
              "guard, shortcuts off: bound Enter on the VOX button is refused (unchanged)");
        check(!refuseTxKeyActivation(button, &returnPress, false, keys),
              "guard, shortcuts off: an unbound Return is not refused (unchanged)");
        check(!refuseTxKeyActivation(button, &spacePress, true, keys)
                  && !refuseTxKeyActivation(button, &enterPress, true, keys),
              "guard, shortcuts on: nothing is refused (unchanged)");
    }

    qInstallMessageHandler(previousHandler);
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
