#include "TestSettingsProfile.h"
#include "core/backends/flex/FlexWanSession.h"
#include "core/backends/flex/FlexStatusIngress.h"
#include "core/backends/flex/SmartLinkClient.h"
#include "core/backends/flex/SmartLinkCredentials.h"
#include "core/control/RadioConnectionTarget.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <utility>

namespace AetherSDR {
class FlexWanSessionTestAccess {
public:
    static void transport(FlexWanSession& session, int& binds, int& stops, QStringList& dials, bool bindOk = true)
    {
        session.m_prepareReceive = [&binds, bindOk](const WanRadioInfo&) {
            ++binds;
            return ReceiveTransportResult{bindOk, quint16(bindOk ? 49152 : 0)};
        };
        session.m_stopReceive = [&stops] { ++stops; };
        session.m_dial = [&dials](const QString&, quint16, const QString& handle) { dials.append(handle); };
    }
    static quint64 epoch(const FlexWanSession& session) { return session.m_epoch; }
    static void deadline(FlexWanSession& session) { QMetaObject::invokeMethod(&session.m_deadline, "timeout", Qt::DirectConnection); }
    static WanConnection& wire(FlexWanSession& session) { return session.m_connection; }
    static void certificate(FlexWanSession& session) { session.m_certificateAttempt = session.m_epoch; }
};
struct SmartLinkClientTestAccess {
    static SmartLinkCredentials& credentials(SmartLinkClient& client) { return client.m_credentials; }
    static void authenticated(SmartLinkClient& client) { client.m_authenticated = true; client.m_serverConnected = true; }
    static void sent(SmartLinkClient& client) { client.m_connectSent = true; }
    static void reply(SmartLinkClient& client, const QString& serial, const QString& handle)
    { client.parseConnectReady(QStringLiteral("radio connect_ready serial=%1 handle=%2").arg(serial, handle)); }
    static bool brokerRetired(const SmartLinkClient& client) { return !client.m_serverConnected && client.m_readBuffer.isEmpty(); }
};
struct SmartLinkAccountModelTestAccess {
    static SmartLinkClient& client(SmartLinkAccountModel& model) { return *model.m_client; }
};
struct SmartLinkCredentialsTestAccess {
    static void execute(SmartLinkCredentials& store, SmartLinkCredentials::Execute executor)
    { store.m_state->execute = std::move(executor); }
};
}

using namespace AetherSDR;
namespace {
class Account final : public RemoteRadioAccount {
public:
    struct Request { quint64 lease; Ready ready; Failed failed; QList<quint32> evictions; quint16 port; };
    QList<Request> requests;
    QList<quint64> cancellations;
    quint64 beginConnection(const QString&, quint16 port, const QList<quint32>& handles, Ready ready, Failed failed) override
    {
        const quint64 lease = quint64(requests.size() + 1);
        requests.append({lease, std::move(ready), std::move(failed), handles, port});
        return lease;
    }
    void cancelConnection(quint64 lease) override { cancellations.append(lease); }
};
RemoteRadioRequest request(const std::shared_ptr<Account>& account)
{
    RemoteRadioRequest result;
    result.account = account;
    result.radio.serial = QStringLiteral("test-radio");
    result.radio.publicIp = QStringLiteral("192.0.2.1");
    result.radio.publicTlsPort = 4994;
    result.radio.publicUdpPort = 4993;
    return result;
}
}

class FlexWanSessionTest : public QObject {
    Q_OBJECT
private slots:
    void headlessReservationCoversPendingCredentialAndCancellation()
    {
        RadioModel model;
        auto target = control::makeModelRadioConnectionTarget(&model);
        QVERIFY(target);
        SmartLinkCredentials::Completion pending;
        SmartLinkClient& client = SmartLinkAccountModelTestAccess::client(model.smartLinkAccounts());
        SmartLinkCredentialsTestAccess::execute(SmartLinkClientTestAccess::credentials(client),
            [&](auto, const QString&, const QString&, auto completion) { pending = std::move(completion); });
        QVERIFY(target->connectConfiguredRemote(QStringLiteral("legacy"), QStringLiteral("selected")));
        QCOMPARE(target->state(), control::RadioConnectionTarget::State::Connecting);
        QVERIFY(pending);
        QVERIFY(!target->connectConfiguredRemote(QStringLiteral("legacy"), QStringLiteral("another")));
        target->disconnectRadio();
        QTRY_COMPARE(target->state(), control::RadioConnectionTarget::State::Idle);
        pending(false, {});
        emit model.smartLinkAccounts().radioListReceived({});
        QCoreApplication::sendPostedEvents();
        QCOMPARE(target->state(), control::RadioConnectionTarget::State::Idle);
        QVERIFY(!model.isConnectAttemptInFlight());
        QVERIFY(!model.isConnected());
    }

    void cancellationRetiresBrokerAndUdpBeforeReplacement()
    {
        FlexStatusIngress ingress;
        int binds = 0, stops = 0;
        QStringList dials;
        auto account = std::make_shared<Account>();
        FlexWanSession session(nullptr, &ingress);
        FlexWanSessionTestAccess::transport(session, binds, stops, dials);
        QSignalSpy ended(&session, &FlexWanSession::disconnected);
        session.connectRadio(request(account));
        QCOMPARE(binds, 1);
        QCOMPARE(account->requests.size(), 1);
        QCOMPARE(account->requests.first().port, quint16(49152));
        const auto old = account->requests.first();
        session.disconnectRadio();
        QCOMPARE(ended.size(), 1);
        QCOMPARE(account->cancellations, QList<quint64>{old.lease});
        QVERIFY(stops > 0);
        session.connectRadio(request(account));
        old.ready(old.lease, QStringLiteral("obsolete"));
        old.failed(old.lease, QStringLiteral("obsolete failure"));
        QVERIFY(dials.isEmpty());
        QVERIFY(session.active());
        const auto next = account->requests.last();
        next.ready(next.lease, QStringLiteral("current"));
        QTRY_COMPARE(dials, QStringList{QStringLiteral("current")});
    }

    void failedBindNeverRequestsBrokerHandle()
    {
        FlexStatusIngress ingress;
        int binds = 0, stops = 0;
        QStringList dials;
        auto account = std::make_shared<Account>();
        FlexWanSession session(nullptr, &ingress);
        FlexWanSessionTestAccess::transport(session, binds, stops, dials, false);
        QSignalSpy errors(&session, &FlexWanSession::errorOccurred);
        session.connectRadio(request(account));
        QCOMPARE(errors.size(), 1);
        QVERIFY(!session.active());
        QVERIFY(account->requests.isEmpty());
        QVERIFY(dials.isEmpty());
    }

    void cancellationDuringStartingDoesNotAllocateTransport()
    {
        FlexStatusIngress ingress;
        int binds = 0, stops = 0;
        QStringList dials;
        auto account = std::make_shared<Account>();
        FlexWanSession session(nullptr, &ingress);
        FlexWanSessionTestAccess::transport(session, binds, stops, dials);
        connect(&session, &FlexWanSession::starting, &session, [&] { session.disconnectRadio(); });
        session.connectRadio(request(account));
        QCOMPARE(binds, 0);
        QVERIFY(account->requests.isEmpty());
        QVERIFY(!session.active());
    }

    void timeoutAndQueuedLifecycleCannotEndReplacement()
    {
        FlexStatusIngress ingress;
        int binds = 0, stops = 0;
        QStringList dials;
        auto account = std::make_shared<Account>();
        FlexWanSession session(nullptr, &ingress);
        FlexWanSessionTestAccess::transport(session, binds, stops, dials);
        session.connectRadio(request(account));
        emit FlexWanSessionTestAccess::wire(session).disconnected();
        FlexWanSessionTestAccess::deadline(session);
        QVERIFY(!session.active());
        session.connectRadio(request(account));
        QCoreApplication::sendPostedEvents();
        QVERIFY(session.active());
        QCOMPARE(account->requests.size(), 2);
    }

    void brokerIsSingleFlightAndRejectsWrongOrRepeatedReply()
    {
        SmartLinkClient client;
        SmartLinkClientTestAccess::authenticated(client);
        QStringList handles;
        const auto ready = [&handles](quint64, const QString& handle) { handles.append(handle); };
        const auto failed = [](quint64, const QString&) {};
        const quint64 first = client.beginConnection(QStringLiteral("radio"), 49152, {}, ready, failed);
        QVERIFY(first);
        QVERIFY(client.accountLocked());
        QCOMPARE(client.beginConnection(QStringLiteral("radio"), 49153, {}, ready, failed), quint64(0));
        SmartLinkClientTestAccess::sent(client);
        SmartLinkClientTestAccess::reply(client, QStringLiteral("wrong-radio"), QStringLiteral("wrong"));
        QVERIFY(handles.isEmpty());
        SmartLinkClientTestAccess::reply(client, QStringLiteral("radio"), QStringLiteral("first"));
        SmartLinkClientTestAccess::reply(client, QStringLiteral("radio"), QStringLiteral("duplicate"));
        QCOMPARE(handles, QStringList{QStringLiteral("first")});
        client.cancelConnection(first);
        QVERIFY(!client.accountLocked());
        QVERIFY(client.isAuthenticated());
        QVERIFY(SmartLinkClientTestAccess::brokerRetired(client));
        SmartLinkClientTestAccess::reply(client, QStringLiteral("radio"), QStringLiteral("late"));
        QCOMPARE(handles.size(), 1);
    }

    void credentialCleanupSurvivesOwnerDestruction()
    {
        using Store = SmartLinkCredentials;
        QList<Store::Kind> started;
        QList<Store::Completion> completions;
        bool delivered = false;
        {
            Store store([&](Store::Kind kind, const QString&, const QString&, Store::Completion complete) {
                started.append(kind);
                completions.append(std::move(complete));
            });
            store.read(QStringLiteral("legacy"), [&](bool, const QString&) { delivered = true; });
            store.write(QStringLiteral("legacy"), QStringLiteral("fixture-token"));
            store.remove(QStringLiteral("legacy"));
        }
        completions.takeFirst()(true, QStringLiteral("obsolete-token"));
        QVERIFY(!delivered);
        QCOMPARE(started.last(), Store::Kind::Write);
        completions.takeFirst()(true, {});
        QCOMPARE(started.last(), Store::Kind::Remove);
        completions.takeFirst()(true, {});
        QVERIFY(completions.isEmpty());
    }

    void credentialDeleteFollowsOutstandingWriteAndStaleReadIsRefused()
    {
        using Store = SmartLinkCredentials;
        QList<Store::Kind> started;
        QList<Store::Completion> completions;
        Store store([&](Store::Kind kind, const QString&, const QString&, Store::Completion complete) {
            started.append(kind); completions.append(std::move(complete));
        });
        store.write(QStringLiteral("legacy"), QStringLiteral("fixture-token"));
        store.remove(QStringLiteral("legacy"));
        QCOMPARE(started, QList<Store::Kind>{Store::Kind::Write});
        completions.takeFirst()(true, {});
        QCOMPARE(started, (QList<Store::Kind>{Store::Kind::Write, Store::Kind::Remove}));
        completions.takeFirst()(true, {});
        bool readAccepted = true;
        store.read(QStringLiteral("legacy"), [&](bool ok, const QString& token) { readAccepted = ok || !token.isEmpty(); });
        store.remove(QStringLiteral("legacy"));
        completions.takeFirst()(true, QStringLiteral("old-token"));
        QVERIFY(!readAccepted);
        QCOMPARE(started.last(), Store::Kind::Remove);
        completions.takeFirst()(true, {});
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("flex-wan-session"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    FlexWanSessionTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "flex_wan_session_test.moc"
