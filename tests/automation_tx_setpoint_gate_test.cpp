// RF power and Tune power are drive, not keying (#5851, reading B): without TX
// permission every bridge surface still sets them, and the power ceiling, not
// permission, bounds them. dumpTree marks them "txSetpoint".
// No radio, no socket, no transmission: the real TxApplet on a bare RadioModel.

#include "TestEventLoop.h"
#include "TestSettingsProfile.h"
#include "core/AutomationServer.h"
#include "core/TxKeyingMarker.h"
#include "gui/TxApplet.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
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
