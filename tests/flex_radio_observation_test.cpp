#include "FlexRadioStatusTestSupport.h"
#include "core/backends/sim/SimBackend.h"
#include "TestSettingsProfile.h"
#include <QCoreApplication>
#include <QEvent>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <memory>
#include <thread>

namespace AetherSDR {
// Our client handle, as the H line sets it.
class RadioConnectionSessionTestAccess {
public:
    static void handle(RadioConnection& connection, quint32 handle) { connection.m_handle = handle; }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {
struct Fixture {
    RadioModel model;
    FlexBackend* backend{dynamic_cast<FlexBackend*>(model.backend())};
    Fixture() { RadioConnectionSessionTestAccess::handle(*backend->connection(), 0x42); }
    void report(const QString& object, const QMap<QString, QString>& fields = {})
    { observeRadioStatus(*backend, object, fields); }
    // A decoded report stamped with the given status epoch, as the ingress emits it.
    void stamped(quint64 epoch, const QString& object, const QMap<QString, QString>& fields = {})
    {
        auto value = decodeFlexRadioStatus(object, fields);
        value->statusEpoch = epoch;
        backend->acceptRadioStatus(*value);
    }
};
}

class FlexRadioObservationTest : public QObject {
    Q_OBJECT
private slots:
    void partialAndInvalidFields()
    {
        const auto value = decodeFlexRadioStatus(QStringLiteral("radio"), {
            {"nickname", ""}, {"slices", "-1"}, {"cal_freq", "nan"},
            {"lineout_gain", "101"}, {"headphone_gain", "17"}, {"mf_enable", "garbage"},
            {"full_duplex_enabled", "0"}, {"freq_error_ppb", "-9"}, {"pll_done", "1"}});
        QVERIFY(value);
        QCOMPARE(value->radio.nickname, std::optional<QString>(QString()));
        QVERIFY(!value->radio.slicesAvailable);
        QVERIFY(!value->radio.calFreqMhz);
        QVERIFY(!value->radio.lineoutGain);
        QVERIFY(!value->radio.multiFlexEnabled);
        QCOMPARE(value->radio.headphoneGain, std::optional<int>(17));
        QCOMPARE(value->radio.fullDuplex, std::optional<bool>(false));
        QCOMPARE(value->radio.freqErrorPpb, std::optional<int>(-9));
        QCOMPARE(value->radio.pllDone, std::optional<bool>(true));
        for (const QString& object : {"client 0x0 connected", "client 0x100000000 connected",
                                     "client 0xZZ connected", "client 0x99 surprise"}) {
            QVERIFY(!decodeFlexRadioStatus(object, {}));
        }
        const auto huge = decodeFlexRadioStatus("radio", {{"nickname", QString(4097, 'x')}});
        QVERIFY(huge && !huge->radio.nickname);
    }

    void staticNetworkAndOscillatorDeltas()
    {
        Fixture f;
        f.report("radio static_net_params", {{"ip", "192.0.2.2"}, {"gateway", "192.0.2.1"}});
        f.report("radio static_net_params", {{"netmask", "255.255.255.0"}});
        QCOMPARE(f.model.staticIp(), QString("192.0.2.2"));
        QCOMPARE(f.model.staticGateway(), QString("192.0.2.1"));
        f.report("radio static_net_params", {{"ip", ""}});
        QVERIFY(!f.model.hasStaticIp());
        QCOMPARE(f.model.staticGateway(), QString("192.0.2.1"));
        f.report("radio oscillator", {{"setting", "tcxo"}, {"locked", "2"}, {"gpsdo_present", "1"}});
        QCOMPARE(f.model.oscSetting(), QString("tcxo"));
        QVERIFY(f.model.oscLocked());
        QVERIFY(f.model.gpsdoPresent());
        f.report("radio oscillator", {{"gnss_present", "0"}, {"locked", "broken"}});
        QVERIFY(!f.model.gpsdoPresent());
        QVERIFY(f.model.oscLocked());
        FlexRadioStatusTestPeer::reset(f.model);
        QVERIFY(f.model.oscState().isEmpty());
        QVERIFY(!f.model.oscLocked());
        QVERIFY(f.model.staticGateway().isEmpty());
    }

    void clientUpdatesAndDiscoveryPrecedence()
    {
        Fixture f;
        f.model.setKnownGuiClients({"0x99"}, {"Old"}, {"Seed"});
        f.report("client 0x99 connected", {{"client_id", "identity"}, {"station", "Shack"},
                 {"program", "SmartSDR"}, {"local_ptt", "1"}, {"ip", "192.0.2.9"}});
        f.report("client 0x99 connected", {{"station", "Updated"}});
        QCOMPARE(f.model.clientInfoMap().value(0x99).program, QString("SmartSDR"));
        // A full connected report is the whole state (FlexLib Radio.cs ParseClientStatus).
        QVERIFY(!f.model.clientInfoMap().value(0x99).localPtt);
        f.report("client 0x99", {{"local_ptt", "1"}});
        QVERIFY(f.model.clientInfoMap().value(0x99).localPtt);
        f.model.mergeKnownGuiClients({"0x99"}, {"Stale"}, {"Stale"});
        QCOMPARE(f.model.clientInfoMap().value(0x99).station, QString("Updated"));
        f.report("client 0x99", {{"local_ptt", "0"}});
        QVERIFY(!f.model.clientInfoMap().value(0x99).localPtt);
        f.report("client 0x88", {{"local_ptt", "1"}});
        QVERIFY(!f.model.clientInfoMap().contains(0x88));
        f.report("client 0x99 disconnected");
        f.model.mergeKnownGuiClients({"0x99"}, {"Stale"}, {"Stale"});
        QVERIFY(!f.model.clientInfoMap().contains(0x99));
        f.report("client 0x99 connected", {{"station", "Replacement"}});
        QCOMPARE(f.model.clientInfoMap().value(0x99).station, QString("Replacement"));
    }

    void disconnectPublishesAfterCleanup()
    {
        Fixture f;
        f.report("client 0x99 connected", {{"station", "Old"}});
        FlexRadioStatusTestPeer::foreign(f.model, 1, 0x99);
        FlexRadioStatusTestPeer::foreign(f.model, 2, 0x99);
        bool complete = true;
        connect(&f.model, &RadioModel::slotOccupancyChanged, &f.model, [&](int id) {
            complete &= !f.model.clientInfoMap().contains(0x99)
                && FlexRadioStatusTestPeer::foreign(f.model, 2) == 0;
            if (id == 1) { FlexRadioStatusTestPeer::foreign(f.model, 1, 0x77); }
        });
        f.report("client 0x99 disconnected");
        QVERIFY(complete);
        QCOMPARE(FlexRadioStatusTestPeer::foreign(f.model, 1), quint32(0x77));
        QCOMPARE(FlexRadioStatusTestPeer::foreign(f.model, 2), quint32(0));
    }

    void noticeCannotFollowReusedHandle()
    {
        Fixture f;
        QSignalSpy notices(&f.model, &RadioModel::clientConnected);
        f.report("client 0x99 connected", {{"station", "Old"}});
        QTest::qWait(450);
        f.report("client 0x99 disconnected");
        f.report("client 0x99 connected", {{"station", "New"}});
        QTest::qWait(400);
        QCOMPARE(notices.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(notices.count(), 1, 1000);
        QCOMPARE(notices.first().at(2).toString(), QString("New"));
        f.report("client 0x99 connected", {{"station", "New"}});
        QTest::qWait(800);
        QCOMPARE(notices.count(), 1);
    }

    // A forced disconnect the radio reports for us lands before the session
    // ends; nothing stamped with an ended or earlier session applies.
    void terminalReportPrecedesSessionEnd()
    {
        Fixture f;
        QSignalSpy terminal(&f.model, &RadioModel::forcedDisconnectRequested);
        const quint64 first = FlexRadioStatusTestPeer::beginConnect(f.model);
        f.stamped(first, "client 0x42 disconnected", {{"forced", "1"}});
        QCOMPARE(terminal.count(), 1);
        QVERIFY(FlexRadioStatusTestPeer::intentional(f.model));
        const quint64 second = FlexRadioStatusTestPeer::beginConnect(f.model);
        f.stamped(first, "radio", {{"nickname", "stale"}});
        QVERIFY(second != first && f.model.nickname() != QString("stale"));
        FlexRadioStatusTestPeer::disconnected(f.model);
        f.stamped(second, "radio", {{"nickname", "late"}});
        QVERIFY(f.model.nickname() != QString("late"));
    }

    void newSessionRejectsOldTerminal()
    {
        Fixture f;
        QSignalSpy terminal(&f.model, &RadioModel::forcedDisconnectRequested);
        const quint64 old = FlexRadioStatusTestPeer::beginConnect(f.model);
        FlexRadioStatusTestPeer::beginConnect(f.model);
        f.stamped(old, "client 0x42 disconnected", {{"forced", "1"}});
        QCOMPARE(terminal.count(), 0);
    }

    void reentrantRadioChangeStopsOldApplication()
    {
        Fixture f;
        const QString original = f.model.nickname();
        connect(&f.model, &RadioModel::callsignChanged, &f.model, [&] { FlexRadioStatusTestPeer::beginConnect(f.model); });
        f.report("radio", {{"callsign", "NEW"}, {"nickname", "must-not-apply"}});
        QCOMPARE(f.model.nickname(), original);
    }

    void repeatedObservationsRemainVisible()
    {
        Fixture f;
        QSignalSpy progress(&f.model, &RadioModel::frequencyCalibrationReported);
        f.report("radio", {{"pll_done", "0"}});
        f.report("radio", {{"pll_done", "0"}});
        f.report("radio", {{"pll_done", "1"}});
        QCOMPARE(progress.count(), 3);
        QCOMPARE(progress.last().first().toBool(), true);
    }

    // The ingress decodes each report once and still hands the line on.
    void ingressDecodesOnceAndPassesTheLineOn()
    {
        FlexBackend backend;
        QSignalSpy reports(&backend, &IRadioBackend::radioStatusObserved);
        QSignalSpy accepted(backend.statusIngress(), &FlexStatusIngress::lineAccepted);
        backend.statusIngress()->deliverForTest(CommandParser::parseLine(
            QStringLiteral("S00000042|client 0x99 connected station=Shack local_ptt=1")));
        QCOMPARE(reports.count(), 1);
        const auto report = qvariant_cast<RadioStatusObservation>(reports.first().first());
        QCOMPARE(report.client.handle, quint32(0x99));
        QCOMPARE(report.client.station, std::optional<QString>(QStringLiteral("Shack")));
        QCOMPARE(report.statusEpoch, backend.statusIngress()->epoch());
        QCOMPARE(accepted.count(), 1);
    }

    // A line the connection read in a session that has since been replaced
    // never reaches the model; the new session's does.
    void queuedLineCannotResurrectPreviousSession()
    {
        Fixture f;
        RadioConnection* transport = f.backend->connection();
        const quint64 old = FlexRadioStatusTestPeer::beginConnect(f.model);
        emit transport->lineReceived(CommandParser::parseLine(QStringLiteral("S00000042|radio nickname=queued-old")), old);
        const quint64 current = FlexRadioStatusTestPeer::beginConnect(f.model);
        QCoreApplication::sendPostedEvents();
        QVERIFY(f.model.nickname() != QString("queued-old"));
        emit transport->lineReceived(CommandParser::parseLine(QStringLiteral("S00000042|radio nickname=current")), current);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(f.model.nickname(), QString("current"));
    }

    // SmartLink radio and client status reach the model through
    // connectViaWan's ingress route.
    void smartLinkStatusRoutesThroughTheIngress()
    {
        Fixture f;
        WanConnection wan;
        f.model.connectViaWan(&wan, QStringLiteral("192.0.2.1"), 4993);
        emit wan.lineReceived(CommandParser::parseLine(QStringLiteral("S00000042|radio nickname=remote")), wan.lineEpoch());
        emit wan.lineReceived(CommandParser::parseLine(QStringLiteral("S00000042|radio oscillator state=gpsdo setting=auto locked=1")),
                              wan.lineEpoch());
        QCoreApplication::sendPostedEvents();
        QCOMPARE(f.model.nickname(), QString("remote"));
        QCOMPARE(f.model.oscState(), QString("gpsdo"));
        f.model.disconnectFromRadio();
    }

    // Demo keeps its own radio-global values but takes the oscillator,
    // client and static-network reports its synthetic wire sends.
    void demoRelaysOscillatorButNotRadioGlobals()
    {
        RadioModel model;
        QVERIFY(model.rebuildBackendForTest(QStringLiteral("sim")));
        auto* sim = qobject_cast<SimBackend*>(model.backend());
        QVERIFY(sim);
        const QString nickname = model.nickname();
        sim->statusIngress()->deliverForTest(CommandParser::parseLine(
            QStringLiteral("S00000042|radio oscillator state=tcxo setting=tcxo locked=1 tcxo_present=1")));
        sim->statusIngress()->deliverForTest(CommandParser::parseLine(QStringLiteral("S00000042|radio nickname=wire")));
        QCOMPARE(model.oscState(), QString("tcxo"));
        QCOMPARE(model.oscSetting(), QString("tcxo"));
        QVERIFY(model.oscLocked());
        QCOMPARE(model.nickname(), nickname);
    }

    void queuedWanCallbackRejectsDestroyedTransport()
    {
        Fixture f;
        auto wan = std::make_unique<WanConnection>();
        f.model.connectViaWan(wan.get(), QString(), 0);
        QSignalSpy changed(&f.model, &RadioModel::infoChanged);
        std::thread producer([&wan] { emit wan->versionReceived(QStringLiteral("4.3.16")); });
        producer.join();
        wan.reset();
        QCoreApplication::sendPostedEvents(&f.model, QEvent::MetaCall);
        QVERIFY(!f.model.isWan());
        QCOMPARE(changed.count(), 0);
    }

    void staleCapabilityPublicationCannotOverwriteNewerModel()
    {
        Fixture f;
        bool nested = false;
        connect(f.backend, &IRadioBackend::radioStatusObserved, &f.model,
                [&](const RadioStatusObservation& observation) {
            if (observation.radio.model && !nested) {
                nested = true;
                f.report("radio", {{"model", "FLEX-6600"}});
            }
        });
        QSignalSpy capabilities(f.backend, &IRadioBackend::capabilitiesChanged);
        f.report("radio", {{"model", "FLEX-6700"}});
        QCOMPARE(capabilities.count(), 1);
        f.report("radio", {{"model", "FLEX-6600"}});
        QCOMPARE(capabilities.count(), 1);
    }

    void unrelatedNestedReportDoesNotHideCapabilityChange()
    {
        Fixture f;
        connect(f.backend, &IRadioBackend::radioStatusObserved, &f.model,
                [&](const RadioStatusObservation& observation) {
            if (observation.radio.model) { f.report("radio", {{"nickname", "Nested"}}); }
        });
        QSignalSpy capabilities(f.backend, &IRadioBackend::capabilitiesChanged);
        f.report("radio", {{"model", "FLEX-6700"}});
        QCOMPARE(capabilities.count(), 1);
    }

    // A model name RadioModel did not apply announces no capability revision.
    void droppedModelReportAnnouncesNothing()
    {
        Fixture f;
        FlexRadioStatusTestPeer::beginConnect(f.model);
        FlexRadioStatusTestPeer::closeSession(f.model);
        QSignalSpy capabilities(f.backend, &IRadioBackend::capabilitiesChanged);
        f.report("radio", {{"model", "FLEX-6700"}});
        QVERIFY(f.model.model() != QString("FLEX-6700"));
        QCOMPARE(capabilities.count(), 0);
    }

    // A full "connected" report without local_ptt clears it.
    void connectedReportResetsLocalPtt()
    {
        Fixture f;
        f.report("client 0x99 connected", {{"station", "Shack"}, {"local_ptt", "1"}});
        QVERIFY(f.model.clientInfoMap().value(0x99).localPtt);
        f.report("client 0x99 connected", {{"station", "Shack"}});
        QVERIFY(!f.model.clientInfoMap().value(0x99).localPtt);
    }

    // A late disconnect (a new connect already begun) keeps the new
    // session's client inventory.
    void lateDisconnectKeepsNewSessionClients()
    {
        Fixture f;
        FlexRadioStatusTestPeer::beginConnect(f.model);
        FlexRadioStatusTestPeer::beginLateConnect(f.model);
        f.report("client 0x99 connected", {{"station", "New"}});
        FlexRadioStatusTestPeer::disconnected(f.model);
        QCOMPARE(f.model.clientInfoMap().value(0x99).station, QString("New"));
        f.report("client 0x99", {{"local_ptt", "1"}});
        QVERIFY(f.model.clientInfoMap().value(0x99).localPtt);
    }

    // The connection thread posts a forced-disconnect line and then its
    // disconnect; the model sees them in that order, so the notice applies
    // before the session ends.
    void forcedNoticeLandsBeforeTransportDisconnect()
    {
        Fixture f;
        QSignalSpy terminal(&f.model, &RadioModel::forcedDisconnectRequested);
        const quint64 epoch = FlexRadioStatusTestPeer::beginConnect(f.model);
        RadioConnection* transport = f.backend->connection();
        QMetaObject::invokeMethod(transport, [transport, epoch] {
            emit transport->lineReceived(CommandParser::parseLine(
                QStringLiteral("S00000042|client 0x42 disconnected forced=1")), epoch);
            emit transport->disconnected();
        }, Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents();
        QCoreApplication::sendPostedEvents();
        QCOMPARE(terminal.count(), 1);
    }

    void inventoryIsBoundedButRemovalStillWorks()
    {
        Fixture f;
        for (quint32 id = 1000; id < 1300; ++id) {
            f.report(QString("client 0x%1 connected").arg(id, 0, 16), {{"station", "Test"}});
        }
        QCOMPARE(f.model.clientInfoMap().size(), qsizetype(256));
        f.report("client 0x3e8 disconnected");
        QCOMPARE(f.model.clientInfoMap().size(), qsizetype(255));
        f.report("client 0x600 connected", {{"station", "Test"}});
        QCOMPARE(f.model.clientInfoMap().size(), qsizetype(256));
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile settings("flex-radio-observations");
    QCoreApplication app(argc, argv);
    qRegisterMetaType<RadioStatusObservation>();
    FlexRadioObservationTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "flex_radio_observation_test.moc"
