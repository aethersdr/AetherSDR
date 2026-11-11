// Production menus -> SliceModel -> RadioModel -> injected Kenwood transport.
// No sockets, firmware peer, credentials, RF or hardware acceptance.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "gui/RxApplet.h"
#include "gui/VfoWidget.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QtTest>
#include <memory>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& backend) {
        backend.m_clock.start(); backend.m_protocol.start("fixture-user", "fixture-password", false, 0);
        backend.m_protocol.receive("##CN1;", 1); backend.m_protocol.receive("##ID1;", 2); backend.ready();
    }
    static void message(KenwoodBackend& backend, const QByteArray& bytes) { backend.adopt(bytes); }
    // A local uncertainty transition can withdraw RX truth without fabricating TX.
    static void withdrawReceiveTruth(KenwoodBackend& backend) { backend.m_txStateKnown = false; }
};
}
namespace {
using namespace AetherSDR;
struct Fixture {
    QList<QByteArray> writes;
    RadioModel model;
    kenwood::KenwoodBackend* backend = nullptr;
    std::unique_ptr<RxApplet> rx;
    std::unique_ptr<VfoWidget> vfo;
    explicit Fixture(bool seedAntenna = true) {
        replaceBackend(seedAntenna);
        rx = std::make_unique<RxApplet>(); vfo = std::make_unique<VfoWidget>();
        bindWidgets();
    }
    void observe(const QByteArray& bytes) { kenwood::KenwoodBackendTestAccess::message(*backend, bytes); }
    void replaceBackend(bool seedAntenna = true) {
        auto owned = std::make_unique<kenwood::KenwoodBackend>(kenwood::KenwoodBackend::Transport{
            [this](const QByteArray& bytes) { writes.append(bytes); return true; }, {}});
        backend = owned.get(); model.setBackendForTest(std::move(owned), QStringLiteral("kenwood"));
        kenwood::KenwoodBackendTestAccess::authenticate(*backend);
        for (const QByteArray& truth : {QByteArray("ID024"), QByteArray("XV0"), QByteArray("FR0"), QByteArray("FT1"),
            QByteArray("FA00014074000"), QByteArray("FB00007074000"), QByteArray("OM03"),
            QByteArray("SF0000140740003"), QByteArray("SF1000070740003"), QByteArray("TS0"), QByteArray("RX")}) {
            observe(truth);
        }
        if (seedAntenna) { observe("AN1000"); }
        // setBackendForTest omits setupBackend's actual connection lifecycle relay.
        emit model.connectionStateChanged(true);
    }
    void bindWidgets() {
        rx->setRadioModel(&model); vfo->setRadioModel(&model);
        rx->setSlice(model.slice(0)); vfo->setSlice(model.slice(0));
        rx->show(); vfo->show(); QCoreApplication::processEvents();
    }
    QWidget* surface(bool useVfo) const { return useVfo ? static_cast<QWidget*>(vfo.get()) : static_cast<QWidget*>(rx.get()); }
    QPushButton* button(bool useVfo) const {
        QWidget* widget = surface(useVfo);
        if (!widget) { return nullptr; }
        for (QPushButton* candidate : widget->findChildren<QPushButton*>()) {
            if (candidate->accessibleName() == QLatin1String("RX antenna")) { return candidate; }
        }
        return nullptr;
    }
    QMenu* open(bool useVfo) {
        QPushButton* choose = button(useVfo);
        if (!choose || !choose->isEnabled()) { return nullptr; }
        QTest::mouseClick(choose, Qt::LeftButton); QCoreApplication::processEvents();
        for (QMenu* menu : choose->findChildren<QMenu*>()) { if (menu->isVisible()) { return menu; } }
        return nullptr;
    }
};
QAction* choice(QMenu* menu, const QString& token) {
    if (!menu) { return nullptr; }
    for (QAction* action : menu->actions()) { if (action->data().toString() == token) { return action; } }
    return nullptr;
}
void activate(QMenu* menu, QAction* action) {
    const QPointer<QMenu> alive(menu);
    action->trigger();
    if (alive) { alive->close(); }
    QCoreApplication::processEvents();
}
}

class KenwoodRxAntennaWidgetTest : public QObject {
    Q_OBJECT
private slots:
    void actualReceiveMenu_data() {
        QTest::addColumn<bool>("useVfo"); QTest::newRow("RxApplet") << false; QTest::newRow("VfoWidget") << true;
    }
    void actualReceiveMenu() {
        QFETCH(bool, useVfo);
        Fixture f;
        QVERIFY(f.model.slice(0)); QVERIFY(f.button(false)); QVERIFY(f.button(true));
        QCOMPARE(f.model.slice(0)->rxAntenna(), QStringLiteral("ANT1"));
        const QString rxText = f.button(false)->text(), vfoText = f.button(true)->text();
        const QString rxTip = f.button(false)->toolTip(), vfoTip = f.button(true)->toolTip();
        QMenu* menu = f.open(useVfo); QVERIFY(menu);
        QAction* action = choice(menu, QStringLiteral("RX-ANT")); QVERIFY(action); QVERIFY(action->isEnabled());
        f.writes.clear(); activate(menu, action);
        QCOMPARE(f.writes, QList<QByteArray>({"AN9199;", "AN;"}));
        QCOMPARE(f.model.slice(0)->rxAntenna(), QStringLiteral("ANT1"));
        QCOMPARE(f.button(false)->text(), rxText); QCOMPARE(f.button(true)->text(), vfoText);
        QCOMPARE(f.button(false)->toolTip(), rxTip); QCOMPARE(f.button(true)->toolTip(), vfoTip);
        f.observe("AN1100");
        QCOMPARE(f.model.slice(0)->rxAntenna(), QStringLiteral("RX-ANT"));
        QVERIFY(f.button(false)->text() != rxText); QVERIFY(f.button(true)->text() != vfoText);
        QVERIFY(f.button(false)->toolTip().contains(QStringLiteral("RX-ANT")));
        QVERIFY(f.button(true)->toolTip().contains(QStringLiteral("RX-ANT")));
        QCOMPARE(f.writes, QList<QByteArray>({"AN9199;", "AN;"}));
    }
    void unknownDefaultAndKnownSame_data() { actualReceiveMenu_data(); }
    void unknownDefaultAndKnownSame() {
        QFETCH(bool, useVfo); Fixture f(false);
        QCOMPARE(f.model.slice(0)->rxAntenna(), QStringLiteral("ANT1"));
        QMenu* menu = f.open(useVfo); QVERIFY(menu);
        QAction* action = choice(menu, QStringLiteral("ANT1")); QVERIFY(action);
        f.writes.clear(); activate(menu, action);
        // This preserves the existing explicit mixed-main selection semantics.
        QCOMPARE(f.writes, QList<QByteArray>({"AN1099;", "AN;"}));
        QCOMPARE(f.model.slice(0)->rxAntenna(), QStringLiteral("ANT1"));
        f.observe("AN1000");
        menu = f.open(useVfo); QVERIFY(menu); action = choice(menu, QStringLiteral("ANT1")); QVERIFY(action);
        f.writes.clear(); activate(menu, action);
        QVERIFY(f.writes.isEmpty()); QCOMPARE(f.model.slice(0)->rxAntenna(), QStringLiteral("ANT1"));
    }
    void frequencyReadbackRetiresAntennaAuthority_data() {
        QTest::addColumn<bool>("useVfo");
        QTest::addColumn<bool>("stateReport");
        for (bool useVfo : {false, true}) {
            for (bool stateReport : {false, true}) {
                const QByteArray name = (useVfo ? QByteArray("Vfo-") : QByteArray("Rx-"))
                    + (stateReport ? "SF" : "FA");
                QTest::newRow(name.constData()) << useVfo << stateReport;
            }
        }
    }
    void frequencyReadbackRetiresAntennaAuthority() {
        QFETCH(bool, useVfo); QFETCH(bool, stateReport); Fixture f;
        const quint64 receiverEpoch = f.model.slice(0)->receiveSessionGeneration();
        const quint64 filterEpoch = f.model.slice(0)->receiveFilterContextGeneration();
        const auto chooseSame = [&] {
            QMenu* menu = f.open(useVfo);
            QVERIFY(menu);
            QAction* action = choice(menu, QStringLiteral("ANT1"));
            QVERIFY(action);
            f.writes.clear(); activate(menu, action);
        };
        f.observe(stateReport ? "SF0000140740003" : "FA00014074000");
        chooseSame(); QVERIFY(f.writes.isEmpty());
        f.observe("FB00021074000");
        chooseSame(); QVERIFY(f.writes.isEmpty());
        f.observe(stateReport ? "SF0000070740003" : "FA00007074000");
        QCOMPARE(f.model.slice(0)->receiveSessionGeneration(), receiverEpoch);
        QCOMPARE(f.model.slice(0)->receiveFilterContextGeneration(), filterEpoch);
        QCOMPARE(f.model.slice(0)->rxAntenna(), QStringLiteral("ANT1"));
        chooseSame();
        QCOMPARE(f.writes, QList<QByteArray>({"AN1099;", "AN;"}));
        f.observe("AN1000");
        chooseSame(); QVERIFY(f.writes.isEmpty());
    }
    void fullyRequalifiedSourceMenu_data() { actualReceiveMenu_data(); }
    void fullyRequalifiedSourceMenu() {
        QFETCH(bool, useVfo); Fixture f;
        QMenu* old = f.open(useVfo); QVERIFY(old); QAction* stale = choice(old, QStringLiteral("RX-ANT")); QVERIFY(stale);
        const quint64 epoch = f.model.slice(0)->receiveSessionGeneration();
        f.observe("FR1"); f.observe("SF1000070740003"); f.observe("AN1000");
        QVERIFY(f.model.slice(0)->receiveSessionGeneration() != epoch);
        f.writes.clear(); activate(old, stale);
        QVERIFY(f.writes.isEmpty()); QCOMPARE(f.model.slice(0)->rxAntenna(), QStringLiteral("ANT1"));
        QMenu* fresh = f.open(useVfo); QVERIFY(fresh); QAction* valid = choice(fresh, QStringLiteral("RX-ANT")); QVERIFY(valid);
        f.writes.clear(); activate(fresh, valid);
        QCOMPARE(f.writes, QList<QByteArray>({"AN9199;", "AN;"}));
    }
    void reentrantNativeSelection_data() {
        QTest::addColumn<bool>("useVfo"); QTest::addColumn<int>("change");
        for (bool vfo : {false, true}) {
            for (int change = 0; change < 3; ++change) {
                const QByteArray name = (vfo ? QByteArray("Vfo-") : QByteArray("Rx-")) + QByteArray::number(change);
                QTest::newRow(name.constData()) << vfo << change;
            }
        }
    }
    void reentrantNativeSelection() {
        QFETCH(bool, useVfo); QFETCH(int, change); Fixture f;
        QMenu* menu = f.open(useVfo); QVERIFY(menu); QAction* action = choice(menu, QStringLiteral("RX-ANT")); QVERIFY(action);
        bool emitted = false;
        const auto external = [&](int) {
            if (emitted) { return; } emitted = true;
            if (change == 0) { if (useVfo) { f.vfo.reset(); } else { f.rx.reset(); } }
            if (change == 1) { if (useVfo) { f.vfo->setSlice(nullptr); } else { f.rx->setSlice(nullptr); } }
            if (change == 2) { f.observe("FR1"); f.observe("SF1000070740003"); f.observe("AN1000"); }
        };
        if (useVfo) { QObject::connect(f.vfo.get(), &VfoWidget::flexRxAntennaSelected, &f.model, external); }
        else { QObject::connect(f.rx.get(), &RxApplet::flexRxAntennaSelected, &f.model, external); }
        f.writes.clear(); activate(menu, action);
        QVERIFY(emitted);
        // Source requalification itself produces queries; no antenna setter may follow it.
        for (const QByteArray& bytes : f.writes) { QVERIFY(!bytes.startsWith("AN9") && !bytes.startsWith("AN1") && !bytes.startsWith("AN2")); }
        QCOMPARE(f.model.slice(0)->rxAntenna(), QStringLiteral("ANT1"));
        if (change < 2) { QVERIFY(f.writes.isEmpty()); }
    }
    void unsafeLatchedMenu_data() {
        QTest::addColumn<bool>("useVfo"); QTest::addColumn<int>("reason");
        for (bool vfo : {false, true}) {
            for (int reason = 0; reason < 4; ++reason) {
                const QByteArray name = (vfo ? QByteArray("Vfo-") : QByteArray("Rx-")) + QByteArray::number(reason);
                QTest::newRow(name.constData()) << vfo << reason;
            }
        }
    }
    void unsafeLatchedMenu() {
        QFETCH(bool, useVfo); QFETCH(int, reason);
        Fixture f;
        QMenu* menu = f.open(useVfo); QVERIFY(menu);
        QAction* action = choice(menu, QStringLiteral("RX-ANT")); QVERIFY(action);
        const QString rxText = f.button(false)->text(), vfoText = f.button(true)->text();
        if (reason == 0) { kenwood::KenwoodBackendTestAccess::withdrawReceiveTruth(*f.backend); }
        if (reason == 1) { f.observe("TS1"); }
        if (reason == 2) { f.observe("TX0"); }
        if (reason == 3) { f.observe("FR1"); } // No fresh physical SF/mode qualification yet.
        f.writes.clear(); activate(menu, action);
        QVERIFY(f.writes.isEmpty()); QCOMPARE(f.model.slice(0)->rxAntenna(), QStringLiteral("ANT1"));
        QCOMPARE(f.button(false)->text(), rxText); QCOMPARE(f.button(true)->text(), vfoText);
    }
    void retiredBackendMenu_data() { actualReceiveMenu_data(); }
    void retiredBackendMenu() {
        QFETCH(bool, useVfo); Fixture f;
        QMenu* menu = f.open(useVfo); QVERIFY(menu);
        QPointer<QAction> oldAction(choice(menu, QStringLiteral("RX-ANT"))); QVERIFY(oldAction);
        QPointer<IRadioBackend> oldBackend(f.backend);
        // The owning window detaches widgets before retiring their raw slice binding.
        f.rx->setSlice(nullptr); f.vfo->setSlice(nullptr);
        f.replaceBackend(); f.bindWidgets(); QVERIFY(!oldBackend);
        f.writes.clear();
        if (oldAction) { activate(menu, oldAction); }
        QVERIFY(f.writes.isEmpty()); QCOMPARE(f.model.slice(0)->rxAntenna(), QStringLiteral("ANT1"));
        QVERIFY(f.button(false)->toolTip().contains(QStringLiteral("ANT1")));
        QVERIFY(f.button(true)->toolTip().contains(QStringLiteral("ANT1")));
    }
    void menuOwnerLifetime_data() { actualReceiveMenu_data(); }
    void menuOwnerLifetime() {
        QFETCH(bool, useVfo); Fixture f;
        QPointer<QMenu> menu(f.open(useVfo)); QVERIFY(menu);
        QPointer<QAction> action(choice(menu, QStringLiteral("RX-ANT"))); QVERIFY(action);
        f.writes.clear();
        if (useVfo) { f.vfo.reset(); } else { f.rx.reset(); }
        QVERIFY(!menu); QVERIFY(!action); QCoreApplication::processEvents();
        QVERIFY(f.writes.isEmpty()); QCOMPARE(f.model.slice(0)->rxAntenna(), QStringLiteral("ANT1"));
    }
};

int main(int argc, char** argv) {
    TestSettingsProfile profile(QStringLiteral("kenwood-rx-antenna-widget"));
    if (!profile.isValid()) { return 1; }
    QApplication app(argc, argv); AppSettings::instance().load();
    KenwoodRxAntennaWidgetTest test; return QTest::qExec(&test, argc, argv);
}

#include "kenwood_rx_antenna_widget_test.moc"
