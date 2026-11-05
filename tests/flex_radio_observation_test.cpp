#include "FlexRadioStatusTestSupport.h"
#include "TestSettingsProfile.h"
#include <QCoreApplication>
#include <QEvent>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <memory>
#include <thread>

using namespace AetherSDR;

namespace {
struct Fixture {
    RadioModel model;
    FlexRadioStatusSource source;
    FlexBackend* backend{dynamic_cast<FlexBackend*>(model.backend())};
    Fixture() { FlexRadioStatusTestPeer::handle(*backend->connection(), 0x42); }
    void report(const QString& object, const QMap<QString, QString>& fields = {})
    {
        const auto value = decodeFlexRadioStatus(object, fields, source.lifetime(), 0x42);
        if (value) { backend->acceptRadioStatus(*value); }
    }
};
}

class FlexRadioObservationTest : public QObject {
    Q_OBJECT
private slots:
    void partialAndInvalidFields()
    {
        FlexRadioStatusSource source;
        const auto value = decodeFlexRadioStatus(QStringLiteral("radio"), {
            {"nickname", ""}, {"slices", "-1"}, {"cal_freq", "nan"},
            {"lineout_gain", "101"}, {"headphone_gain", "17"}, {"mf_enable", "garbage"},
            {"full_duplex_enabled", "0"}, {"freq_error_ppb", "-9"}, {"pll_done", "1"}}, source.lifetime());
        QVERIFY(value);
        QCOMPARE(value->radio.nickname, std::optional<QString>(QString()));
        QVERIFY(!value->radio.slicesAvailable);
        QVERIFY(!value->radio.calFreqMhz);
        QVERIFY(!value->radio.lineoutGain);
        QVERIFY(!value->radio.multiFlexEnabled);
        QCOMPARE(value->radio.headphoneGain, std::optional<int>(17));
        QCOMPARE(value->radio.fullDuplex, std::optional<bool>(false));
        QCOMPARE(value->radio.freqErrorPpb, std::optional<int>(-9));
        QCOMPARE(value->calibrationDone, std::optional<bool>(true));
        for (const QString& object : {"client 0x0 connected", "client 0x100000000 connected",
                                     "client 0xZZ connected", "client 0x99 surprise"}) {
            QVERIFY(!decodeFlexRadioStatus(object, {}, source.lifetime()));
        }
        const auto huge = decodeFlexRadioStatus("radio", {{"nickname", QString(4097, 'x')}}, source.lifetime());
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

    void retiredOrdinaryAndOrderedTerminalDelivery()
    {
        Fixture f;
        QSignalSpy terminal(&f.model, &RadioModel::forcedDisconnectRequested);
        const auto data = decodeFlexRadioStatus("radio", {{"nickname", "stale"}}, f.source.lifetime(), 0x42);
        const auto forced = decodeFlexRadioStatus("client 0x42 disconnected", {{"forced", "1"}}, f.source.lifetime(), 0x42);
        const auto end = f.source.end();
        QVERIFY(data && forced && end);
        QVERIFY(!data->current());
        QVERIFY(forced->current());
        f.backend->acceptRadioStatus(*data);
        QVERIFY(f.model.nickname() != QString("stale"));
        f.backend->acceptRadioStatus(*forced);
        QCOMPARE(terminal.count(), 1);
        QVERIFY(FlexRadioStatusTestPeer::intentional(f.model));
        f.backend->acceptRadioStatus(*end);
        QVERIFY(!forced->current());
        f.backend->acceptRadioStatus(*forced);
        QCOMPARE(terminal.count(), 1);
    }

    void newSessionRejectsOldTerminal()
    {
        Fixture f;
        QSignalSpy terminal(&f.model, &RadioModel::forcedDisconnectRequested);
        const auto old = decodeFlexRadioStatus("client 0x42 disconnected", {{"forced", "1"}}, f.source.lifetime(), 0x42);
        f.source.begin();
        QVERIFY(old && !old->current());
        f.backend->acceptRadioStatus(*old);
        QCOMPARE(terminal.count(), 0);
    }

    void reentrantRadioChangeStopsOldApplication()
    {
        Fixture f;
        const QString original = f.model.nickname();
        connect(&f.model, &RadioModel::callsignChanged, &f.model, [&] { f.source.begin(); });
        f.report("radio", {{"callsign", "NEW"}, {"nickname", "must-not-apply"}});
        QCOMPARE(f.model.nickname(), original);
    }

    void repeatedObservationsRemainVisible()
    {
        Fixture f;
        QSignalSpy progress(&f.model, &RadioModel::calibrationProgress);
        f.report("radio", {{"pll_done", "0"}});
        f.report("radio", {{"pll_done", "0"}});
        f.report("radio", {{"pll_done", "1"}});
        QCOMPARE(progress.count(), 3);
        QCOMPARE(progress.last().first().toBool(), true);
    }

    void bothTransportsDecodeAndPreserveResponse()
    {
        RadioConnection lan;
        WanConnection wan;
        QSignalSpy lanReports(&lan, &RadioConnection::radioStatusObserved);
        QSignalSpy wanReports(&wan, &WanConnection::radioStatusObserved);
        QSignalSpy responses(&lan, &RadioConnection::commandResponse);
        const QString status("S00000042|client 0x99 connected station=Shack local_ptt=1");
        FlexRadioStatusTestPeer::line(lan, status);
        FlexRadioStatusTestPeer::line(wan, status);
        QCOMPARE(lanReports.count(), 1);
        QCOMPARE(wanReports.count(), 1);
        const auto a = qvariant_cast<RadioStatusObservation>(lanReports.first().first());
        const auto b = qvariant_cast<RadioStatusObservation>(wanReports.first().first());
        QCOMPARE(a.client.handle, b.client.handle);
        QCOMPARE(a.client.station, b.client.station);
        FlexRadioStatusTestPeer::end(lan);
        FlexRadioStatusTestPeer::line(lan, "R12|0|removed");
        QCOMPARE(responses.count(), 1);
        QVERIFY(!a.current());
        FlexRadioStatusTestPeer::awaitingCertificate(wan, true);
        FlexRadioStatusTestPeer::line(wan, status);
        QCOMPARE(wanReports.count(), 1);
    }

    void ingressCapturesBeforeReentrantCallbacks()
    {
        RadioConnection connection;
        QSignalSpy reports(&connection, &RadioConnection::radioStatusObserved);
        connect(&connection, &RadioConnection::messageReceived, &connection,
                [&](const ParsedMessage&) { FlexRadioStatusTestPeer::begin(connection); });
        FlexRadioStatusTestPeer::line(connection, "S00000042|radio nickname=old");
        QCOMPARE(reports.count(), 0);
    }

    void cancellationRetiresUnseenIngress()
    {
        Fixture f;
        const auto source = f.backend->connection()->radioStatusLifetime();
        const auto queued = decodeFlexRadioStatus("radio", {{"nickname", "old"}}, source);
        QVERIFY(queued && queued->current());
        f.backend->invalidateRadioStatus();
        QVERIFY(!queued->current());
        f.backend->acceptRadioStatus(*queued);
        QVERIFY(f.model.nickname() != QString("old"));
    }

    void workerQueueIsFencedAndDeliveredOnOwnerThread()
    {
        Fixture f;
        int reports = 0;
        bool ownerThread = true;
        connect(f.backend, &IRadioBackend::radioStatusObserved, &f.model,
                [&](const RadioStatusObservation& observation) {
            if (observation.kind != RadioStatusObservation::Kind::Radio) { return; }
            ++reports;
            ownerThread &= QThread::currentThread() == f.model.thread();
        });
        RadioConnection* transport = f.backend->connection();
        QMetaObject::invokeMethod(transport, [transport] {
            FlexRadioStatusTestPeer::line(*transport, "S00000042|radio nickname=queued-old");
        }, Qt::BlockingQueuedConnection);
        f.backend->invalidateRadioStatus();
        QCoreApplication::processEvents();
        QCOMPARE(reports, 0);
        QMetaObject::invokeMethod(transport, [transport] {
            FlexRadioStatusTestPeer::begin(*transport);
            FlexRadioStatusTestPeer::line(*transport, "S00000042|radio nickname=current");
        }, Qt::BlockingQueuedConnection);
        QTRY_COMPARE(reports, 1);
        QVERIFY(ownerThread);
        QCOMPARE(f.model.nickname(), QString("current"));
    }

    void reusedWanAdoptsNewSourceAndRejectsOldQueue()
    {
        Fixture f;
        WanConnection wan;
        f.backend->bindRadioStatus(&wan);
        const auto old = decodeFlexRadioStatus("radio", {{"nickname", "old"}}, wan.radioStatusLifetime());
        FlexRadioStatusTestPeer::begin(wan);
        const auto current = wan.radioStatusLifetime();
        f.backend->bindRadioStatus(&wan);
        QVERIFY(current->active.load());
        QVERIFY(old && !old->current());
        FlexRadioStatusTestPeer::line(wan, "S00000042|radio nickname=new");
        QCOMPARE(f.model.nickname(), QString("new"));
        f.backend->acceptRadioStatus(*old);
        QCOMPARE(f.model.nickname(), QString("new"));
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
