// RF power and Tune power are drive, not keying (#5851, reading B): without TX
// permission every bridge surface still sets them, and the power ceiling, not
// permission, bounds them. dumpTree marks them "txSetpoint".
// No radio, no socket, no transmission: the real TxApplet on a bare RadioModel.

#include "RadioSetupControlFixture.h"
#include "TestEventLoop.h"
#include "TestSettingsProfile.h"
#include "core/AutomationServer.h"
#include "core/ShortcutManager.h"
#include "core/TxKeyingMarker.h"
#include "gui/SliderLeaseTxGate.h"
#include "gui/TxBandDialog.h"
#include "gui/TxApplet.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
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
    // AETHER_AUTOMATION_TX_MAX_POWER as start() reads it; -1 disarms.
    static void setTxMaxPower(AutomationServer& server, int ceiling) { server.m_txMaxPower = ceiling; }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {

// A host with no automation handler: the bridge falls back to these, which
// answer as MainWindow's meta-object entries do for a drive action: they cannot
// see the power ceiling, so they assume it armed and refuse.
class FallbackMainWindow : public QMainWindow {
    Q_OBJECT
public:
    Q_INVOKABLE int fireShortcutAction(const QString&, bool) { return 5; }
    Q_INVOKABLE int injectKeyEventForAutomation(const QString&, bool press, bool)
    {
        return press ? 6 : 0;
    }
};

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

    // ── No TX permission: drive is not keying, every surface still sets it ──
    // Reading B of #5851: TX permission gates keying only; the power ceiling
    // bounds drive. Each verb must reach TransmitModel without permission.
    for (const Setpoint& sp : setpoints) {
        const QByteArray n = sp.name.toUtf8();
        const int target = sp.model() == 37 ? 38 : 37;
        check(ok(cmd(server, QStringLiteral("invoke"), sp.name, QStringLiteral("setValue"),
                     QString::number(target)))
                  && AetherTest::waitFor([&] { return sp.model() == target; }, 2000),
              "TX not allowed: invoke setValue on " + n + " reaches TransmitModel");
        const int beforeWheel = sp.model();
        check(ok(cmd(server, QStringLiteral("invoke"), sp.name, QStringLiteral("wheel"),
                     QStringLiteral("1")))
                  && AetherTest::waitFor([&] { return sp.model() != beforeWheel; }, 2000),
              "TX not allowed: invoke wheel on " + n + " moves the setpoint");
        const QString w = QString::number(sp.slider->width() - 2);
        const QString h = QString::number(sp.slider->height() / 2);
        const int beforeClick = sp.model();
        check(ok(cmd(server, QStringLiteral("clickAt"), sp.name, {}, w + QLatin1Char(' ') + h))
                  && AetherTest::waitFor([&] { return sp.model() != beforeClick; }, 2000),
              "TX not allowed: clickAt on the " + n + " groove moves the setpoint");
        check(ok(cmd(server, QStringLiteral("doubleClick"), sp.name, {}, w + QLatin1Char(' ') + h)),
              "TX not allowed: doubleClick on " + n + " is accepted");
        check(ok(cmd(server, QStringLiteral("wheel"), sp.name, {}, h + QLatin1Char(' ') + h
                     + QStringLiteral(" 3"))),
              "TX not allowed: wheel on " + n + " is accepted");
        check(ok(cmd(server, QStringLiteral("drag"), sp.name, {}, QStringLiteral("40 0"))),
              "TX not allowed: drag on " + n + " is accepted");
    }
    {
        const QJsonObject rf = cmd(server, QStringLiteral("transmit"), {}, QStringLiteral("rfpower"),
                                   QStringLiteral("41"));
        const QJsonObject tune = cmd(server, QStringLiteral("transmit"), {},
                                     QStringLiteral("tunepower"), QStringLiteral("42"));
        check(ok(rf) && ok(tune) && tx.rfPower() == 41 && tx.tunePower() == 42,
              "TX not allowed: transmit rfpower and tunepower set drive");
    }

    // ── No TX permission, ceiling armed: the ceiling, not permission, bounds drive ──
    AutomationServerTestAccess::setTxMaxPower(server, 10);
    for (const Setpoint& sp : setpoints) {
        const QByteArray n = sp.name.toUtf8();
        check(ok(cmd(server, QStringLiteral("invoke"), sp.name, QStringLiteral("setValue"),
                     QStringLiteral("80")))
                  && AetherTest::waitFor([&] { return sp.model() == 10; }, 2000),
              "TX not allowed, ceiling 10: invoke setValue 80 on " + n + " is clamped to 10");
    }
    {
        const QJsonObject rf = cmd(server, QStringLiteral("transmit"), {}, QStringLiteral("rfpower"),
                                   QStringLiteral("80"));
        check(ok(rf) && tx.rfPower() == 10 && rf.value(QStringLiteral("clampedTo")).toInt() == 10,
              "TX not allowed, ceiling 10: transmit rfpower 80 is clamped to 10");
    }
    AutomationServerTestAccess::setTxMaxPower(server, -1);

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
        // Reading B (#5851): TX permission gates keying only, so the drive
        // shortcuts are refused only while the power ceiling is armed, with or
        // without permission, a step down too. The handlers mirror
        // fireShortcutAction / injectKeyEventForAutomation: bridgeRefusesTxDrive
        // on the ceiling they are handed.
        ShortcutManager::Action rfUp;
        rfUp.id = QStringLiteral("rf_power_up");
        rfUp.setsTxDrive = true;
        check(ShortcutManager::bridgeRefusesTxDrive(rfUp, /*ceiling=*/true),
              "shortcut gate: ceiling armed: a setsTxDrive action is refused");
        check(!ShortcutManager::bridgeRefusesTxDrive(rfUp, /*ceiling=*/false),
              "shortcut gate: ceiling off: a setsTxDrive action runs, whatever TX permission is");
        bool ceilingSeen = false;
        int fired = 0;
        server.setShortcutAutomationHandler([&](const QString&, bool, bool ceiling,
                                                const std::shared_ptr<TxController>&) {
            ceilingSeen = ceiling;
            if (ShortcutManager::bridgeRefusesTxDrive(rfUp, ceiling)) { return 5; }
            ++fired;
            return 0;
        });
        server.setKeyEventAutomationHandler([&](const QString&, bool press, bool,
                                                bool ceiling, const std::shared_ptr<TxController>&) {
            ceilingSeen = ceiling;
            if (press && ShortcutManager::bridgeRefusesTxDrive(rfUp, ceiling)) { return 6; }
            ++fired;
            return 0;
        });
        const auto shortcut = [&] {
            return AutomationServerTestAccess::request(server,
                QJsonObject{{QStringLiteral("cmd"), QStringLiteral("shortcut")},
                            {QStringLiteral("target"), QStringLiteral("rf_power_up")}});
        };
        const auto ceilingBlocked = [](const QJsonObject& r, const QString& target) {
            return !ok(r) && r.value(QStringLiteral("error")).toString().startsWith(
                QStringLiteral("blocked: '") + target
                + QStringLiteral("' would set transmit drive past the power ceiling "
                                 "(AETHER_AUTOMATION_TX_MAX_POWER=10)"));
        };
        server.setTxAllowed(false);
        AutomationServerTestAccess::setTxMaxPower(server, 10);
        const QJsonObject armedShortcut = shortcut();
        check(ceilingSeen && fired == 0 && ceilingBlocked(armedShortcut, QStringLiteral("rf_power_up")),
              "shortcut: TX not allowed, ceiling armed: rf_power_up is refused as past the ceiling");
        const QJsonObject armedKey = cmd(server, QStringLiteral("keyevent"), {},
                                         QStringLiteral("press"), QStringLiteral("rf_power_up"));
        check(ceilingSeen && fired == 0 && ceilingBlocked(armedKey, QStringLiteral("rf_power_up")),
              "keyevent: TX not allowed, ceiling armed: an rf_power_up press is refused as past the ceiling");
        check(ok(cmd(server, QStringLiteral("keyevent"), {}, QStringLiteral("release"),
                     QStringLiteral("rf_power_up"))),
              "keyevent: TX not allowed, ceiling armed: the release is still delivered");
        AutomationServerTestAccess::setTxMaxPower(server, -1);
        fired = 0;
        check(ok(shortcut()) && !ceilingSeen && fired == 1,
              "shortcut: TX not allowed, ceiling off: rf_power_up fires: drive is not keying");
        check(ok(cmd(server, QStringLiteral("keyevent"), {}, QStringLiteral("press"),
                     QStringLiteral("rf_power_up"))) && !ceilingSeen && fired == 2,
              "keyevent: TX not allowed, ceiling off: an rf_power_up press is delivered");
        server.setTxAllowed(true);
        AutomationServerTestAccess::setTxMaxPower(server, 10);
        fired = 0;
        check(ceilingBlocked(shortcut(), QStringLiteral("rf_power_up")) && fired == 0,
              "shortcut: TX allowed, ceiling armed: rf_power_up is refused as past the ceiling");
        AutomationServerTestAccess::setTxMaxPower(server, -1);
        server.setShortcutAutomationHandler({});
        server.setKeyEventAutomationHandler({});
        server.setTxAllowed(false);
    }

    // No handler, TX allowed, no ceiling: the bridge falls back to the main
    // window's methods, which cannot be handed the ceiling and refuse drive as
    // if it were armed. The refusal names that cause, not a ceiling of -1. The
    // only main window alive is this one.
    {
        FallbackMainWindow fallback;
        fallback.show();
        server.setTxAllowed(true);
        AutomationServerTestAccess::setTxMaxPower(server, -1);
        const auto noHandler = [](const QJsonObject& r) {
            const QString e = r.value(QStringLiteral("error")).toString();
            return !ok(r) && e.contains(QStringLiteral("no automation handler"))
                && !e.contains(QStringLiteral("AETHER_AUTOMATION_TX_MAX_POWER=-1"));
        };
        check(noHandler(AutomationServerTestAccess::request(server,
                  QJsonObject{{QStringLiteral("cmd"), QStringLiteral("shortcut")},
                              {QStringLiteral("target"), QStringLiteral("rf_power_up")}})),
              "shortcut: no handler, TX allowed: the refusal names the fallback, not the ceiling");
        check(noHandler(cmd(server, QStringLiteral("keyevent"), {}, QStringLiteral("press"),
                            QStringLiteral("rf_power_up"))),
              "keyevent: no handler, TX allowed: the refusal names the fallback, not the ceiling");
        server.setTxAllowed(false);
    }

    // ── An unbound key through the slider keyboard lease (#5851) ───────────
    // eventFilter's lease sets the focused slider from arrows, Page and
    // Home/End whatever they are bound to; the bridge refuses those presses on
    // a drive slider. Real TxApplet sliders, the predicate the injection calls.
    {
        const int leaseKeys[] = {Qt::Key_Left, Qt::Key_Right, Qt::Key_Up, Qt::Key_Down,
                                 Qt::Key_PageUp, Qt::Key_PageDown, Qt::Key_Home, Qt::Key_End};
        for (const Setpoint& sp : setpoints) {
            bool allowed = true;
            bool ceiling = true;
            for (const int k : leaseKeys) {
                allowed = allowed && !bridgeKeyMovesTxSetpoint(sp.slider, k, false);
                ceiling = ceiling && bridgeKeyMovesTxSetpoint(sp.slider, k, true);
            }
            check(allowed, "keyevent lease: ceiling off: arrows, Page and Home/End reach a leased "
                               + sp.name.toUtf8() + ", whatever TX permission is");
            check(ceiling, "keyevent lease: ceiling armed: they are refused on "
                               + sp.name.toUtf8() + ", a step down too (End would jump to maximum)");
            check(!bridgeKeyMovesTxSetpoint(sp.slider, Qt::Key_M, true)
                      && !bridgeKeyMovesTxSetpoint(sp.slider, Qt::Key_Return, true),
                  "keyevent lease: a key the lease does not apply is not refused on "
                      + sp.name.toUtf8());
        }
        QWidget marked;
        markTxSetpoint(&marked);
        QSlider inner(&marked);
        QSlider plain;
        check(bridgeKeyMovesTxSetpoint(&inner, Qt::Key_End, true),
              "keyevent lease: ceiling armed: a slider inside a marked drive control is refused");
        check(!bridgeKeyMovesTxSetpoint(&plain, Qt::Key_End, true)
                  && !bridgeKeyMovesTxSetpoint(nullptr, Qt::Key_End, true),
              "keyevent lease: an unmarked slider, or no lease, is not refused");

        // MainWindow cannot be built here without binding its discovery socket,
        // so the wiring is pinned as text: the check runs before the key is sent.
        QFile src(QStringLiteral(AETHER_SOURCE_DIR "/src/gui/MainWindow_Shortcuts.cpp"));
        const QString text = src.open(QIODevice::ReadOnly) ? QString::fromUtf8(src.readAll()) : QString();
        const qsizetype fn = text.indexOf(QStringLiteral(
            "int MainWindow::injectKeyEventForAutomation(const QString& spec, bool press, bool allowTx,"));
        const qsizetype gate = text.indexOf(QStringLiteral("bridgeKeyMovesTxSetpoint("), fn);
        const qsizetype send = text.indexOf(QStringLiteral("QApplication::sendEvent(this, &ev);"), fn);
        check(fn >= 0 && gate > fn && send > gate,
              "keyevent lease: injectKeyEventForAutomation checks the lease before it sends the key");
        const qsizetype gateEnd = text.indexOf(QLatin1Char(')'), text.indexOf(QLatin1Char(')'), gate) + 1);
        check(gate > fn && text.mid(gate, gateEnd - gate).contains(QStringLiteral("txCeilingArmed")),
              "keyevent lease: the lease check is handed the bridge's power ceiling");
    }

    // ── `tci send` of a drive SET (#5851) ──────────────────────────────────
    // Bounded by the ceiling, not TX permission; refused before any simulator
    // is looked up, so no TCI client is needed.
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
        // Ceiling off, no TX permission: a SET passes the gate and then needs
        // a connected simulator.
        const auto reachesSim = [](const QJsonObject& r) {
            return !ok(r) && r.value(QStringLiteral("error")).toString()
                                 .contains(QStringLiteral("is not connected"));
        };
        check(reachesSim(tciSend(QStringLiteral("drive:0,100;")))
                  && reachesSim(tciSend(QStringLiteral("tune_drive:0,100")))
                  && reachesSim(tciSend(QStringLiteral("vfo:0,0,14074000; DRIVE : 0 , 90;"))),
              "TX not allowed, ceiling off: tci send drive/tune_drive SETs pass the gate");

        // Ceiling armed, no TX permission: a SET above it is refused, at or
        // below it passes the gate (and then needs a connected simulator).
        const auto ceilingBlocked = [](const QJsonObject& r) {
            return !ok(r) && r.value(QStringLiteral("error")).toString()
                                 .contains(QStringLiteral("past the power ceiling"));
        };
        AutomationServerTestAccess::setTxMaxPower(server, 10);
        const QJsonObject over = tciSend(QStringLiteral("drive:0,50;"));
        check(ceilingBlocked(over) && over.value(QStringLiteral("error")).toString().startsWith(
                  QStringLiteral("blocked: 'tci send drive:0,50' would set transmit drive past "
                                 "the power ceiling (AETHER_AUTOMATION_TX_MAX_POWER=10)")),
              "TX not allowed, ceiling 10: tci send drive:0,50 is refused as past the ceiling");
        const QJsonObject tuneOver = tciSend(QStringLiteral("tune_drive:0,11"));
        check(ceilingBlocked(tuneOver),
              "TX not allowed, ceiling 10: tci send tune_drive:0,11 is refused");
        check(over.value(QStringLiteral("error")).toString()
                      .endsWith(QStringLiteral("Use `transmit rfpower <n>`, which clamps."))
                  && tuneOver.value(QStringLiteral("error")).toString()
                         .endsWith(QStringLiteral("Use `transmit tunepower <n>`, which clamps.")),
              "TX not allowed, ceiling 10: a refused drive SET names transmit rfpower, tune_drive "
              "names transmit tunepower");
        check(ceilingBlocked(tciSend(QStringLiteral("vfo:0,0,14074000; drive:0,90;"))),
              "TX not allowed, ceiling 10: a drive SET later in the payload is refused");
        check(ceilingBlocked(tciSend(QStringLiteral("drive:0,5,5;")))
                  && ceilingBlocked(tciSend(QStringLiteral("drive:0,x;"))),
              "TX not allowed, ceiling 10: a drive SET the ceiling cannot read is refused");
        check(!ceilingBlocked(tciSend(QStringLiteral("drive:0,10;")))
                  && !ceilingBlocked(tciSend(QStringLiteral("tune_drive:0,3;")))
                  && !ceilingBlocked(tciSend(QStringLiteral("drive:0;"))),
              "TX not allowed, ceiling 10: a SET at or below it, and a read, pass the gate");
        server.setTxAllowed(true);
        check(ceilingBlocked(tciSend(QStringLiteral("drive:0,50;"))),
              "TX allowed, ceiling 10: tci send drive:0,50 is refused as past the ceiling too");
        AutomationServerTestAccess::setTxMaxPower(server, -1);
        check(reachesSim(tciSend(QStringLiteral("drive:0,50;"))),
              "TX allowed, ceiling off: tci send drive:0,50 passes the gate (unchanged)");
        server.setTxAllowed(false);
    } else {
        std::printf("[SKIP] tci send drive gate: TCI not built (HAVE_WEBSOCKETS off)\n");
    }

    // ── TX Band Settings per-band power edits (#5851) ──────────────────────
    // The radio restores a band's RF/Tune power on QSY into it, so these edits
    // set drive: setText plus a focus change commits through editingFinished.
    {
        RadioModel bandRadio;
        RadioSetupTestBackend* backend = installRadioSetupBackend(bandRadio);
        RadioSetupControlTestAccess::band(bandRadio, 9);
        TxBandDialog dialog(&bandRadio);
        dialog.show();
        AetherTest::waitFor([&] { return dialog.isVisible(); }, 2000);
        const QString names[] = {QStringLiteral("RF power percent, band 20m"),
                                 QStringLiteral("Tune power percent, band 20m")};
        for (const QString& name : names) {
            const QByteArray n = name.toUtf8();
            QLineEdit* edit = nullptr;
            for (auto* e : dialog.findChildren<QLineEdit*>()) {
                if (e->accessibleName() == name) { edit = e; }
            }
            if (!edit) {
                check(false, "TxBandDialog has the " + n + " edit");
                continue;
            }
            check(isTxSetpointControl(edit), n + " is marked a TX setpoint");
        }
        // dumpTree reads the marker, not the name: neither edit is "RF power".
        {
            const QJsonObject bandTree = cmd(server, QStringLiteral("dumpTree"), {}, {}, {});
            for (const QString& name : names) {
                QJsonObject bandNode;
                for (const auto& top : bandTree.value(QStringLiteral("roots")).toArray()) {
                    findNode(top.toObject(), name, bandNode);
                }
                check(bandNode.value(QStringLiteral("txSetpoint")).toBool(),
                      "dumpTree shows " + name.toUtf8() + " as txSetpoint");
            }
        }
        // Drive, not keying (#5851): the edits take bridge input without TX permission.
        backend->commands.clear();
        QLineEdit* rf = nullptr;
        for (auto* e : dialog.findChildren<QLineEdit*>()) {
            if (e->accessibleName() == names[0]) { rf = e; }
        }
        check(rf && ok(cmd(server, QStringLiteral("invoke"), names[0], QStringLiteral("setText"),
                           QStringLiteral("41")))
                  && rf->text() == QStringLiteral("41"),
              "TX not allowed: invoke setText on the band RF power edit is accepted");
        if (rf) { emit rf->editingFinished(); }
        check(backend->commands == QStringList{QStringLiteral("transmit bandset 9 rfpower=41")},
              "TX not allowed: the committed edit is sent as the band's RF power");
        backend->replyLast({RadioSetupReply::Kind::Accepted, 0, {}});

        // Ceiling armed: the edits are clamped like the sliders,
        // keyed on the marker although neither name is "RF power"/"Tune power".
        AutomationServerTestAccess::setTxMaxPower(server, 10);
        const QStringList fields{QStringLiteral("rfpower"), QStringLiteral("tunepower")};
        for (int i = 0; i < 2; ++i) {
            const QByteArray n = names[i].toUtf8();
            QLineEdit* edit = nullptr;
            for (auto* e : dialog.findChildren<QLineEdit*>()) {
                if (e->accessibleName() == names[i]) { edit = e; }
            }
            if (!edit) { continue; }
            backend->commands.clear();
            const QJsonObject set = cmd(server, QStringLiteral("invoke"), names[i],
                                        QStringLiteral("setText"), QStringLiteral("100"));
            check(ok(set) && edit->text() == QStringLiteral("10"),
                  "TX not allowed, ceiling 10: invoke setText 100 on " + n + " is clamped to 10 (now "
                      + edit->text().toUtf8() + ")");
            emit edit->editingFinished();
            check(backend->commands
                      == QStringList{QStringLiteral("transmit bandset 9 ") + fields[i]
                                     + QStringLiteral("=10")},
                  "TX not allowed, ceiling 10: the committed " + n + " edit reaches the radio as 10");
            backend->replyLast({RadioSetupReply::Kind::Accepted, 0, {}});
            check(!ok(cmd(server, QStringLiteral("clickAt"), names[i], {}, QStringLiteral("5 5"))),
                  "TX not allowed, ceiling 10: clickAt on " + n + " is refused");
            // The pointer verbs share one ceiling check (pointerSafetyError); only
            // these targets tell the marker from the name "RF power"/"Tune power".
            const auto pointerBlocked = [](const QJsonObject& r) {
                const QString e = r.value(QStringLiteral("error")).toString();
                return !ok(r) && e.contains(QStringLiteral("pointer input would bypass the power"));
            };
            check(pointerBlocked(cmd(server, QStringLiteral("wheel"), names[i], {},
                                     QStringLiteral("5 5 3")))
                      && pointerBlocked(cmd(server, QStringLiteral("drag"), names[i], {},
                                            QStringLiteral("40 0"))),
                  "TX not allowed, ceiling 10: wheel and drag on " + n
                      + " are refused as bypassing the ceiling");
        }
        AutomationServerTestAccess::setTxMaxPower(server, -1);
        backend->commands.clear();
        check(rf && ok(cmd(server, QStringLiteral("invoke"), names[0], QStringLiteral("setText"),
                           QStringLiteral("60")))
                  && rf->text() == QStringLiteral("60"),
              "TX not allowed, ceiling off: invoke setText 60 on the band RF power edit is not clamped");
        server.setTxAllowed(false);
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

    // TX allowed, ceiling armed: invoke setValue still clamps the sliders and
    // a wheel notch, which has no target value, is refused.
    AutomationServerTestAccess::setTxMaxPower(server, 10);
    for (const Setpoint& sp : setpoints) {
        const QByteArray n = sp.name.toUtf8();
        check(ok(cmd(server, QStringLiteral("invoke"), sp.name, QStringLiteral("setValue"),
                     QStringLiteral("50")))
                  && AetherTest::waitFor([&] { return sp.model() == 10; }, 2000),
              "TX allowed, ceiling 10: invoke setValue 50 on " + n + " is clamped to 10");
        const int sliderAtCeiling = sp.slider->value();
        const QJsonObject wheel = cmd(server, QStringLiteral("invoke"), sp.name,
                                      QStringLiteral("wheel"), QStringLiteral("1"));
        const QString wheelError = wheel.value(QStringLiteral("error")).toString();
        check(!ok(wheel) && wheelError.contains(QStringLiteral("past the power ceiling"))
                  && sp.slider->value() == sliderAtCeiling,
              "TX allowed, ceiling 10: invoke wheel on " + n + " is refused (error: "
                  + wheelError.left(60).toUtf8() + ", slider "
                  + QByteArray::number(sliderAtCeiling) + " -> "
                  + QByteArray::number(sp.slider->value()) + ")");
    }
    AutomationServerTestAccess::setTxMaxPower(server, -1);

    // ── Withdrawing permission does not stop a drive gesture: drive is not keying ──
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

        // Offsets are from the press point: -60 drags RF power well below 50.
        server.setTxAllowed(false);
        const QJsonObject after = gesture(QStringLiteral("move"), {}, QStringLiteral("-60 0"));
        check(ok(after) && tx.rfPower() < 50 && rf->value() == tx.rfPower(),
              "gesture: after permission is withdrawn, move still drags RF power (now "
                  + QByteArray::number(tx.rfPower()) + ")");
        check(ok(gesture(QStringLiteral("end"), {}, {})), "gesture: and end releases it");
    }
    server.setTxAllowed(false);

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

#include "automation_tx_setpoint_gate_test.moc"
