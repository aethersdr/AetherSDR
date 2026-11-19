// Without TX permission the bridge refuses to change RF power or Tune power on
// every surface, as `transmit rfpower|tunepower` refuses (#5851). Reading stays
// open, other setpoints stay free, and with permission every surface works.
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
        const auto refusedAsDrive = [](const QJsonObject& reply) {
            return !ok(reply) && reply.value(QStringLiteral("error")).toString()
                                     .contains(QStringLiteral("sets transmit drive"));
        };
        check(refusedAsDrive(cmd(server, QStringLiteral("invoke"), sp.name,
                                 QStringLiteral("setValue"),
                                 QString::number(before == 37 ? 38 : 37))),
              "TX not allowed: invoke setValue on " + n + " is refused as transmit drive");
        // 1, not 3: invoke wheel takes only -1 or 1, so 3 is refused unguarded.
        check(refusedAsDrive(cmd(server, QStringLiteral("invoke"), sp.name,
                                 QStringLiteral("wheel"), QStringLiteral("1"))),
              "TX not allowed: invoke wheel on " + n + " is refused as transmit drive");
        check(refusedAsDrive(cmd(server, QStringLiteral("clickAt"), sp.name, {},
                                 w + QLatin1Char(' ') + h)),
              "TX not allowed: clickAt on the " + n + " groove is refused as transmit drive");
        check(refusedAsDrive(cmd(server, QStringLiteral("doubleClick"), sp.name, {},
                                 w + QLatin1Char(' ') + h)),
              "TX not allowed: doubleClick on " + n + " is refused as transmit drive");
        check(refusedAsDrive(cmd(server, QStringLiteral("wheel"), sp.name, {},
                                 h + QLatin1Char(' ') + h + QStringLiteral(" 3"))),
              "TX not allowed: wheel on " + n + " is refused as transmit drive");
        check(refusedAsDrive(cmd(server, QStringLiteral("drag"), sp.name, {},
                                 QStringLiteral("40 0"))),
              "TX not allowed: drag on " + n + " is refused as transmit drive");
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
