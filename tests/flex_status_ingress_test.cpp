// Socket-free: the Flex status ingress fence and its wire order, with real
// transports that are never opened, plus RadioModel's production binding.
#include "TestSettingsProfile.h"
#include "core/backends/flex/CommandParser.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/FlexStatusIngress.h"
#include "core/backends/flex/RadioConnection.h"
#include "core/backends/flex/WanConnection.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>
#include <QtTest>

namespace AetherSDR {
class RadioConnectionSessionTestAccess {
public:
    static void beginSession(RadioConnection& connection)
    {
        if (connection.thread() == QThread::currentThread()) {
            connection.resetSessionState();
            return;
        }
        QMetaObject::invokeMethod(&connection, [&connection] { connection.resetSessionState(); },
                                  Qt::BlockingQueuedConnection);
    }
};
class SessionControlTestAccess {
public:
    static quint64 beginConnect(RadioModel& model) { return model.beginStatusSession(); }
    static void connectedAt(RadioModel& model, quint64 epoch) { model.m_connectedStatusEpoch = epoch; }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {
ParsedMessage line(const char* text) { return CommandParser::parseLine(QString::fromLatin1(text)); }
QStringList objects(const QSignalSpy& spy)
{
    QStringList out;
    for (const QList<QVariant>& args : spy) { out << args.at(0).value<ParsedMessage>().object; }
    return out;
}
} // namespace

class FlexStatusIngressTest final : public QObject {
    Q_OBJECT
private slots:
    // A line is accepted only with the current epoch: beginSession() refuses
    // everything queued before it, and retire() closes the current epoch but
    // never a newer one.
    void epochFence()
    {
        RadioConnection connection;
        FlexStatusIngress ingress;
        ingress.bind(&connection);
        QSignalSpy accepted(&ingress, &FlexStatusIngress::lineAccepted);
        emit connection.lineReceived(line("S0|transmit rfpower=1"), ingress.epoch());
        const quint64 first = ingress.beginSession();
        emit connection.lineReceived(line("S0|transmit rfpower=2"), first);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(accepted.size(), 1);   // the pre-session line was refused
        QCOMPARE(accepted.at(0).at(0).value<ParsedMessage>().kvs.value("rfpower"), QStringLiteral("2"));

        emit connection.lineReceived(line("S0|interlock state=READY"), first);
        const quint64 second = ingress.beginSession();   // a reconnect before delivery
        ingress.retire(first);                           // the late end of the first
        emit connection.lineReceived(line("S0|atu status=NONE"), second);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(objects(accepted), QStringList({QStringLiteral("transmit"), QStringLiteral("atu")}));
        ingress.retire(second);
        emit connection.lineReceived(line("S0|apd enable=1"), second);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(accepted.size(), 2);
    }

    // Rebinding replaces the route: lines posted through the old one are
    // refused even when their epoch matches. A live bind stamps its own epoch.
    void routesAreExclusive()
    {
        RadioConnection lan;
        WanConnection wan;
        FlexStatusIngress ingress;
        ingress.bind(&lan);
        QSignalSpy accepted(&ingress, &FlexStatusIngress::lineAccepted);
        emit lan.lineReceived(line("S0|transmit rfpower=3"), ingress.epoch());
        const quint64 live = ingress.bindLive(&wan);
        QCOMPARE(wan.lineEpoch(), live);
        emit lan.lineReceived(line("S0|transmit rfpower=4"), live);
        emit wan.lineReceived(line("S0|atu status=NONE"), wan.lineEpoch());
        QCoreApplication::sendPostedEvents();
        QCOMPARE(objects(accepted), QStringList({QStringLiteral("atu")}));
        emit wan.lineReceived(line("S0|apd enable=1"), wan.lineEpoch());
        ingress.bind(&lan);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(accepted.size(), 1);
    }

    // The H line reaches the model before connected(): both take one queued
    // hop from the connection's thread, in the order the wire produced them.
    void handleLineArrivesBeforeConnected()
    {
        FlexBackend backend;
        RadioConnection* connection = backend.connection();
        RadioConnectionSessionTestAccess::beginSession(*connection);
        QStringList order;
        connect(backend.statusIngress(), &FlexStatusIngress::lineAccepted, this, [&order](const ParsedMessage& m) {
            order << (m.type == MessageType::Handle ? QStringLiteral("H") : m.object);
        });
        connect(&backend, &IRadioBackend::connected, this, [&order] { order << QStringLiteral("connected"); });
        QMetaObject::invokeMethod(connection, [connection] {
            emit connection->lineReceived(line("H00000071"), connection->lineEpoch());
            emit connection->connected();
            emit connection->lineReceived(line("S0|radio callsign=KK7GWY"), connection->lineEpoch());
        }, Qt::BlockingQueuedConnection);
        QTRY_COMPARE(order.size(), 3);
        QCOMPARE(order, QStringList({QStringLiteral("H"), QStringLiteral("connected"), QStringLiteral("radio")}));
    }

    // On SmartLink the lifecycle takes the same queued hop as the lines, so a
    // line the radio sent before closing lands before the session ends, and
    // nothing from the closed session lands after it.
    void wanLifecycleKeepsWireOrder()
    {
        TestSettingsProfile profile(QStringLiteral("flex-status-ingress"));
        QVERIFY(profile.isValid());
        WanConnection wan;
        RadioModel model;
        QVERIFY(model.rebuildBackendForTest(QStringLiteral("flex")));
        model.connectViaWan(&wan, QStringLiteral("192.0.2.1"), 4992);
        const quint64 epoch = wan.lineEpoch();
        emit wan.lineReceived(line("S0|profile global current=Before close"), epoch);
        emit wan.disconnected();
        emit wan.lineReceived(line("S0|profile global current=After close"), epoch);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(model.activeGlobalProfile(), QStringLiteral("Before close"));
    }

    // A disconnect that lands after a new connect has begun ends the old
    // session only: the new connect's lines still reach the model.
    void lateDisconnectKeepsTheNewSession()
    {
        TestSettingsProfile profile(QStringLiteral("flex-status-ingress-late"));
        QVERIFY(profile.isValid());
        RadioModel model;
        QVERIFY(model.rebuildBackendForTest(QStringLiteral("flex")));
        RadioConnection* lan = model.connection();
        SessionControlTestAccess::connectedAt(model, SessionControlTestAccess::beginConnect(model));
        const quint64 next = SessionControlTestAccess::beginConnect(model);   // a quick reconnect
        QVERIFY(QMetaObject::invokeMethod(&model, "onDisconnected", Qt::DirectConnection));
        emit lan->lineReceived(line("S0|profile global current=New session"), next);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(model.activeGlobalProfile(), QStringLiteral("New session"));
    }

    // MainWindow reuses one WanConnection; a disconnect queued by the previous
    // SmartLink session cannot end the session bound after it.
    void reusedWanRefusesEarlierLifecycle()
    {
        TestSettingsProfile profile(QStringLiteral("flex-status-ingress-reuse"));
        QVERIFY(profile.isValid());
        WanConnection wan;
        RadioModel model;
        QVERIFY(model.rebuildBackendForTest(QStringLiteral("flex")));
        model.connectViaWan(&wan, QStringLiteral("192.0.2.1"), 4992);
        emit wan.disconnected();                                  // queued, not yet run
        model.connectViaWan(&wan, QStringLiteral("192.0.2.1"), 4992);   // the next session
        QCoreApplication::sendPostedEvents();
        emit wan.lineReceived(line("S0|profile global current=Second session"), wan.lineEpoch());
        QCoreApplication::sendPostedEvents();
        QCOMPARE(model.activeGlobalProfile(), QStringLiteral("Second session"));
    }

    // Leaving SmartLink for the LAN takes the WAN route with it.
    void leavingWanDropsItsRoute()
    {
        TestSettingsProfile profile(QStringLiteral("flex-status-ingress-leave"));
        QVERIFY(profile.isValid());
        WanConnection wan;
        RadioModel model;
        QVERIFY(model.rebuildBackendForTest(QStringLiteral("flex")));
        model.connectViaWan(&wan, QStringLiteral("192.0.2.1"), 4992);
        QVERIFY(QMetaObject::invokeMethod(&model, "onDisconnected", Qt::DirectConnection));
        emit wan.lineReceived(line("S0|profile global current=Stray"), wan.lineEpoch());
        QCoreApplication::sendPostedEvents();
        QVERIFY(model.activeGlobalProfile() != QLatin1String("Stray"));
    }
};

QTEST_GUILESS_MAIN(FlexStatusIngressTest)
#include "flex_status_ingress_test.moc"
