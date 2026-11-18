// Without TX permission the bridge refuses to change RF power or Tune power on
// every bridge surface, as `transmit rfpower|tunepower` refuses (#5851). Reading stays
// open, other setpoints stay free, and with permission every surface works.
// No radio, no socket, no transmission: the real TxApplet on a bare RadioModel.

#include "TestEventLoop.h"
#include "TestSettingsProfile.h"
#include "core/AutomationServer.h"
#include "core/TxKeyingMarker.h"
#include "gui/SliderLeaseTxGate.h"
#include "gui/TxApplet.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QMainWindow>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>

#include <cstdio>
#include <functional>

namespace AetherSDR {
class AutomationServerTestAccess {
public:
    static QJsonObject request(AutomationServer& server, const QJsonObject& object)
    {
        return server.handleLine(QJsonDocument(object).toJson(QJsonDocument::Compact), nullptr);
    }
    // `gesture` needs an owner identity; an inert socket, never connected.
    static QJsonObject requestAs(AutomationServer& server, const QJsonObject& object,
                                 QLocalSocket* identity)
    {
        return server.handleLine(QJsonDocument(object).toJson(QJsonDocument::Compact), identity);
    }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool ok, const QByteArray& what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what.constData());
    failures += ok ? 0 : 1;
}

bool ok(const QJsonObject& reply) { return reply.value(QStringLiteral("ok")).toBool(); }

QJsonObject cmd(AutomationServer& server, const QString& name, const QString& target,
                const QString& action, const QString& value)
{
    QJsonObject o{{QStringLiteral("cmd"), name}};
    if (!target.isEmpty()) { o[QStringLiteral("target")] = target; }
    if (!action.isEmpty()) { o[QStringLiteral("action")] = action; }
    if (!value.isEmpty()) { o[QStringLiteral("value")] = value; }
    return AutomationServerTestAccess::request(server, o);
}

QSlider* sliderNamed(QWidget& root, const QString& name)
{
    for (auto* s : root.findChildren<QSlider*>()) {
        if (s->accessibleName() == name) { return s; }
    }
    return nullptr;
}

const QJsonObject* findNode(const QJsonObject& node, const QString& name, QJsonObject& out)
{
    if (node.value(QStringLiteral("accessibleName")).toString() == name) {
        out = node;
        return &out;
    }
    for (const auto& child : node.value(QStringLiteral("children")).toArray()) {
        if (findNode(child.toObject(), name, out)) { return &out; }
    }
    return nullptr;
}

struct Setpoint {
    QString name;
    QSlider* slider;
    std::function<int()> model;
};

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-automation-tx-setpoint-gate-test"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "FAIL could not create isolated settings profile\n");
        return 1;
    }
    qunsetenv("AETHER_AUTOMATION_ALLOW_TX");
    qunsetenv("AETHER_AUTOMATION_TX_MAX_POWER");
    QApplication app(argc, argv);

    RadioModel radio;
    TransmitModel& tx = radio.transmitModel();
    TxApplet applet;
    applet.setTransmitModel(&tx);
    applet.resize(300, 500);
    applet.show();
    AetherTest::waitFor([&] { return applet.isVisible(); }, 2000);

    const QList<Setpoint> setpoints{
        {QStringLiteral("RF power"), sliderNamed(applet, QStringLiteral("RF power")),
         [&] { return tx.rfPower(); }},
        {QStringLiteral("Tune power"), sliderNamed(applet, QStringLiteral("Tune power")),
         [&] { return tx.tunePower(); }},
    };
    for (const Setpoint& sp : setpoints) {
        if (!sp.slider) {
            check(false, "TxApplet has the " + sp.name.toUtf8() + " slider");
            return 1;
        }
        check(isTxSetpointControl(sp.slider)
                  && transmitControlMatch(sp.slider) == TransmitControlMatch::None,
              sp.name.toUtf8() + " is marked a TX setpoint and is not a keying control");
    }

    AutomationServer server;
    server.setRadioModel(&radio);
    server.setTxAllowed(false);

    QJsonObject node;
    const QJsonObject tree = cmd(server, QStringLiteral("dumpTree"), {}, {}, {});
    for (const auto& top : tree.value(QStringLiteral("roots")).toArray()) {
        findNode(top.toObject(), QStringLiteral("RF power"), node);
    }
    check(node.value(QStringLiteral("txSetpoint")).toBool()
              && !node.value(QStringLiteral("keying")).toBool(),
          "dumpTree shows RF power as txSetpoint, not keying");

    // ── No TX permission: every surface that can change the value is refused ──
    for (const Setpoint& sp : setpoints) {
        const QByteArray n = sp.name.toUtf8();
        const int before = sp.model();
        const int sliderBefore = sp.slider->value();
        const QString w = QString::number(sp.slider->width() - 2);
        const QString h = QString::number(sp.slider->height() / 2);
        const QJsonObject invoke = cmd(server, QStringLiteral("invoke"), sp.name,
                                       QStringLiteral("setValue"),
                                       QString::number(before == 37 ? 38 : 37));
        check(!ok(invoke) && invoke.value(QStringLiteral("error")).toString()
                                 .contains(QStringLiteral("sets transmit drive")),
              "TX not allowed: invoke setValue on " + n + " is refused as transmit drive");
        check(!ok(cmd(server, QStringLiteral("invoke"), sp.name, QStringLiteral("wheel"),
                      QStringLiteral("3"))),
              "TX not allowed: invoke wheel on " + n + " is refused");
        check(!ok(cmd(server, QStringLiteral("clickAt"), sp.name, {}, w + QLatin1Char(' ') + h)),
              "TX not allowed: clickAt on the " + n + " groove is refused");
        check(!ok(cmd(server, QStringLiteral("doubleClick"), sp.name, {}, w + QLatin1Char(' ') + h)),
              "TX not allowed: doubleClick on " + n + " is refused");
        check(!ok(cmd(server, QStringLiteral("wheel"), sp.name, {}, h + QLatin1Char(' ') + h
                      + QStringLiteral(" 3"))),
              "TX not allowed: wheel on " + n + " is refused");
        check(!ok(cmd(server, QStringLiteral("drag"), sp.name, {}, QStringLiteral("40 0"))),
              "TX not allowed: drag on " + n + " is refused");
        AetherTest::pumpFor(100);
        check(sp.model() == before && sp.slider->value() == sliderBefore,
              "TX not allowed: " + n + " is unchanged in the model and on the slider");
        const QJsonObject read = AutomationServerTestAccess::request(server, QJsonObject{
            {QStringLiteral("cmd"), QStringLiteral("get")},
            {QStringLiteral("model"), QStringLiteral("transmit")}});
        check(ok(read), "TX not allowed: get transmit still reads " + n);
    }
    check(!ok(cmd(server, QStringLiteral("transmit"), {}, QStringLiteral("rfpower"),
                  QStringLiteral("37"))),
          "TX not allowed: transmit rfpower is refused (unchanged)");

    // A setpoint that is not drive stays free: only the two marked sliders gate.
    {
        QSlider other(Qt::Horizontal);
        other.setAccessibleName(QStringLiteral("Phase slider"));
        other.setRange(0, 100);
        other.show();
        check(ok(cmd(server, QStringLiteral("invoke"), QStringLiteral("Phase slider"),
                     QStringLiteral("setValue"), QStringLiteral("12")))
                  && other.value() == 12,
              "TX not allowed: an unmarked slider is still driven (unchanged)");
    }

    // ── The rf_power_up / rf_power_down shortcuts (#5851) ───────────────────
    // The injected results test the bridge's mapping only; the gate that
    // returns them is ShortcutManager::bridgeRefusesTxDrive, tested in
    // control_action_registry_test. Both verbs dispatch only with a main window.
    {
        QMainWindow window;
        window.show();
        bool permission = true;
        server.setShortcutAutomationHandler([&](const QString&, bool allowTx,
                                                const std::shared_ptr<TxController>&) {
            permission = allowTx;
            return 5; // MainWindow::ShortcutFireTxDriveBlocked
        });
        const QJsonObject rep = AutomationServerTestAccess::request(server,
            QJsonObject{{QStringLiteral("cmd"), QStringLiteral("shortcut")},
                        {QStringLiteral("target"), QStringLiteral("rf_power_up")}});
        check(!permission, "shortcut: rf_power_up is dispatched with TX permission off");
        check(!ok(rep) && rep.value(QStringLiteral("error")).toString().startsWith(
                  QStringLiteral("blocked: 'rf_power_up' sets transmit drive, like `transmit rfpower`")),
              "shortcut: a drive-blocked result is refused as transmit drive");
        server.setShortcutAutomationHandler({});

        server.setKeyEventAutomationHandler([&](const QString&, bool press, bool allowTx,
                                                const std::shared_ptr<TxController>&) {
            permission = allowTx;
            return press ? 6 : 0; // MainWindow::KeyInjectTxDriveBlocked on the press
        });
        const QJsonObject key = cmd(server, QStringLiteral("keyevent"), {}, QStringLiteral("press"),
                                    QStringLiteral("rf_power_down"));
        check(!permission && !ok(key) && key.value(QStringLiteral("error")).toString().startsWith(
                  QStringLiteral("blocked: 'rf_power_down' sets transmit drive")),
              "keyevent: a drive-blocked press is refused as transmit drive");
        check(ok(cmd(server, QStringLiteral("keyevent"), {}, QStringLiteral("release"),
                     QStringLiteral("rf_power_down"))),
              "keyevent: the release is delivered");
        server.setKeyEventAutomationHandler({});
    }

    // ── An unbound key through the slider keyboard lease (#5851) ───────────
    // eventFilter's lease sets the focused slider from arrows, Page and
    // Home/End whatever they are bound to; the bridge refuses those presses on
    // a drive slider. Real TxApplet sliders, the predicate the injection calls.
    {
        const int leaseKeys[] = {Qt::Key_Left, Qt::Key_Right, Qt::Key_Up, Qt::Key_Down,
                                 Qt::Key_PageUp, Qt::Key_PageDown, Qt::Key_Home, Qt::Key_End};
        for (const Setpoint& sp : setpoints) {
            bool refused = true;
            bool allowed = true;
            for (const int k : leaseKeys) {
                refused = refused && bridgeKeyMovesTxSetpoint(sp.slider, k, false);
                allowed = allowed && !bridgeKeyMovesTxSetpoint(sp.slider, k, true);
            }
            check(refused, "keyevent lease: arrows, Page and Home/End on a leased "
                               + sp.name.toUtf8() + " are refused without TX permission");
            check(allowed, "keyevent lease: with TX permission they reach " + sp.name.toUtf8());
            check(!bridgeKeyMovesTxSetpoint(sp.slider, Qt::Key_M, false)
                      && !bridgeKeyMovesTxSetpoint(sp.slider, Qt::Key_Return, false),
                  "keyevent lease: a key the lease does not apply is not refused on "
                      + sp.name.toUtf8());
        }
        QWidget marked;
        markTxSetpoint(&marked);
        QSlider inner(&marked);
        QSlider plain;
        check(bridgeKeyMovesTxSetpoint(&inner, Qt::Key_End, false),
              "keyevent lease: a slider inside a marked drive control is refused");
        check(!bridgeKeyMovesTxSetpoint(&plain, Qt::Key_End, false)
                  && !bridgeKeyMovesTxSetpoint(nullptr, Qt::Key_End, false),
              "keyevent lease: an unmarked slider, or no lease, is not refused");

        // MainWindow cannot be built here without binding its discovery socket,
        // so the wiring is pinned as text: the check runs before the key is sent.
        QFile src(QStringLiteral(AETHER_SOURCE_DIR "/src/gui/MainWindow_Shortcuts.cpp"));
        const QString text = src.open(QIODevice::ReadOnly) ? QString::fromUtf8(src.readAll()) : QString();
        const qsizetype fn = text.indexOf(QStringLiteral(
            "int MainWindow::injectKeyEventForAutomation(const QString& spec, bool press, bool allowTx,\n"));
        const qsizetype gate = text.indexOf(QStringLiteral("bridgeKeyMovesTxSetpoint("), fn);
        const qsizetype send = text.indexOf(QStringLiteral("QApplication::sendEvent(this, &ev);"), fn);
        check(fn >= 0 && gate > fn && send > gate,
              "keyevent lease: injectKeyEventForAutomation checks the lease before it sends the key");
    }

    // ── `tci send` of a drive SET (#5851) ──────────────────────────────────
    // Refused before any simulator is looked up, so no TCI client is needed.
    const auto tciSend = [&](const QString& payload) {
        return cmd(server, QStringLiteral("tci"), {}, QStringLiteral("send"), payload);
    };
    const auto driveBlocked = [](const QJsonObject& r) {
        return !ok(r) && r.value(QStringLiteral("error")).toString()
                             .contains(QStringLiteral("sets transmit drive"));
    };
    const bool tciBuilt = !tciSend(QStringLiteral("drive:0;")).value(QStringLiteral("error"))
                               .toString().contains(QStringLiteral("not built"));
    if (tciBuilt) {
        const QJsonObject drive = tciSend(QStringLiteral("drive:0,100;"));
        check(driveBlocked(drive) && drive.value(QStringLiteral("error")).toString().startsWith(
                  QStringLiteral("blocked: 'tci send drive:0,100' sets transmit drive")),
              "TX not allowed: tci send drive:0,100 is refused as transmit drive");
        check(driveBlocked(tciSend(QStringLiteral("tune_drive:0,100"))),
              "TX not allowed: tci send tune_drive:0,100 is refused");
        check(driveBlocked(tciSend(QStringLiteral("vfo:0,0,14074000; DRIVE : 0 , 90;"))),
              "TX not allowed: a drive SET later in a multi-command payload is refused");
        check(!driveBlocked(tciSend(QStringLiteral("drive:0;")))
                  && !driveBlocked(tciSend(QStringLiteral("tune_drive;"))),
              "TX not allowed: tci send drive/tune_drive reads are not refused");
    } else {
        std::printf("[SKIP] tci send drive gate: TCI not built (HAVE_WEBSOCKETS off)\n");
    }

    // ── TX permission: the same calls reach TransmitModel ───────────────────
    server.setTxAllowed(true);
    for (const Setpoint& sp : setpoints) {
        const QByteArray n = sp.name.toUtf8();
        const int target = sp.model() == 37 ? 38 : 37;
        check(ok(cmd(server, QStringLiteral("invoke"), sp.name, QStringLiteral("setValue"),
                     QString::number(target)))
                  && AetherTest::waitFor([&] { return sp.model() == target; }, 2000),
              "TX allowed: invoke setValue on " + n + " reaches TransmitModel");
        const QString w = QString::number(sp.slider->width() - 2);
        const QString h = QString::number(sp.slider->height() / 2);
        check(ok(cmd(server, QStringLiteral("clickAt"), sp.name, {}, w + QLatin1Char(' ') + h)),
              "TX allowed: clickAt on " + n + " is accepted");
    }

    if (tciBuilt) {
        check(!driveBlocked(tciSend(QStringLiteral("drive:0,100;"))),
              "TX allowed: tci send drive:0,100 is not refused as transmit drive");
    }

    // ── A gesture begun with permission stops when permission is withdrawn ──
    {
        QLocalSocket owner;
        auto gesture = [&](const QString& action, const QString& target, const QString& value) {
            QJsonObject o{{QStringLiteral("cmd"), QStringLiteral("gesture")},
                          {QStringLiteral("action"), action}};
            if (!target.isEmpty()) { o[QStringLiteral("target")] = target; }
            if (!value.isEmpty()) { o[QStringLiteral("value")] = value; }
            return AutomationServerTestAccess::requestAs(server, o, &owner);
        };
        QSlider* rf = setpoints.first().slider;
        check(ok(cmd(server, QStringLiteral("invoke"), QStringLiteral("RF power"),
                     QStringLiteral("setValue"), QStringLiteral("50")))
                  && AetherTest::waitFor([&] { return tx.rfPower() == 50; }, 2000),
              "gesture: RF power starts at 50 with the handle centred");
        // Press on the handle itself: a groove press pages and auto-repeats.
        QStyleOptionSlider opt;
        opt.initFrom(rf);
        opt.orientation = rf->orientation();
        opt.minimum = rf->minimum();
        opt.maximum = rf->maximum();
        opt.sliderPosition = rf->sliderPosition();
        opt.sliderValue = rf->value();
        opt.subControls = QStyle::SC_All;
        const QPoint handle = rf->style()->subControlRect(
            QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, rf).center();
        const QJsonObject begin = gesture(QStringLiteral("begin"), QStringLiteral("RF power"),
            QString::number(handle.x()) + QLatin1Char(' ') + QString::number(handle.y()));
        // Read synchronously, with no event-loop turn: a move lands on the
        // slider inside the request, and a held press left idle stops tracking.
        const QJsonObject move = gesture(QStringLiteral("move"), {}, QStringLiteral("20 0"));
        const int moved = tx.rfPower();
        check(ok(begin) && ok(move) && moved > 50 && rf->value() == moved,
              "gesture: with TX permission, begin and move drag RF power up");

        // Offsets are from the press point: -60 would drag RF power well below 50.
        server.setTxAllowed(false);
        const QJsonObject after = gesture(QStringLiteral("move"), {}, QStringLiteral("-60 0"));
        check(!ok(after) && after.value(QStringLiteral("error")).toString()
                                .contains(QStringLiteral("sets transmit drive")),
              "gesture: after permission is withdrawn, move is refused as transmit drive");
        check(tx.rfPower() == moved && rf->value() == moved,
              "gesture: after permission is withdrawn, RF power stays at "
                  + QByteArray::number(moved) + " (now " + QByteArray::number(tx.rfPower()) + ")");
        const QJsonObject status = gesture(QStringLiteral("status"), {}, {});
        check(ok(status) && !status.value(QStringLiteral("active")).toBool(),
              "gesture: the refused move ended the bridge gesture");
    }
    server.setTxAllowed(false);

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
