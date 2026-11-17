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
    static void connectedForCleanup(FlexWanSession& session)
    {
        session.m_connection.m_connected = true;
        session.m_connection.m_commands.begin(++session.m_connection.m_sessionGeneration);
    }
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
    std::function<void()> onCancel;
    quint64 beginConnection(const QString&, quint16 port, const QList<quint32>& handles, Ready ready, Failed failed) override
    {
        const quint64 lease = quint64(requests.size() + 1);
        requests.append({lease, std::move(ready), std::move(failed), handles, port});
        return lease;
    }
    void cancelConnection(quint64 lease) override
    {
        cancellations.append(lease);
        if (onCancel) { onCancel(); }
    }
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
    void teardownReservesOwnershipThroughAccountCallbacks()
    {
        FlexStatusIngress ingress;
        int binds = 0, stops = 0;
        QStringList dials;
        auto account = std::make_shared<Account>();
        FlexWanSession session(nullptr, &ingress);
        FlexWanSessionTestAccess::transport(session, binds, stops, dials);
        session.connectRadio(request(account));
        account->onCancel = [&] {
            QVERIFY(session.active());
            QVERIFY(session.closing());
            session.connectRadio(request(account));
            QCOMPARE(account->requests.size(), 1);
        };
        QSignalSpy ended(&session, &FlexWanSession::disconnected);
        session.disconnectRadio();
        QCOMPARE(ended.size(), 1);
        QVERIFY(!session.active());
        account->onCancel = {};
        session.connectRadio(request(account));
        QCOMPARE(account->requests.size(), 2);
    }

    void replacementCredentialOwnerSharesPendingForgetOrder()
    {
        using Store = SmartLinkCredentials;
        QList<Store::Kind> started;
        QList<Store::Completion> completions;
        {
            Store old;
            SmartLinkCredentialsTestAccess::execute(old,
                [&](auto kind, const QString&, const QString&, auto completion) {
                    started.append(kind);
                    completions.append(std::move(completion));
                });
            old.write(QStringLiteral("legacy"), QStringLiteral("first"));
            old.write(QStringLiteral("legacy"), QStringLiteral("second"));
        }
        Store replacement;
        SmartLinkCredentialsTestAccess::execute(replacement,
            [&](auto kind, const QString&, const QString&, auto completion) {
                started.append(kind);
                completions.append(std::move(completion));
            });
        replacement.remove(QStringLiteral("legacy"));
        QCOMPARE(started.size(), 1);
        completions.takeFirst()(true, {});
        QCOMPARE(started, (QList<Store::Kind>{Store::Kind::Write, Store::Kind::Write}));
        completions.takeFirst()(true, {});
        QCOMPARE(started.last(), Store::Kind::Remove);
        completions.takeFirst()(true, {});
        QVERIFY(completions.isEmpty());
    }

    void cleanupWaitsForRepliesAndRejectsOrdinaryWrites_data()
    {
        QTest::addColumn<bool>("deferred");
        QTest::newRow("normal-close") << false;
        QTest::newRow("forced-close") << true;
    }
    void cleanupWaitsForRepliesAndRejectsOrdinaryWrites()
    {
        QFETCH(bool, deferred);
        FlexStatusIngress ingress;
        int binds = 0, stops = 0;
        QStringList dials, writes;
        FlexWanSession session(nullptr, &ingress);
        FlexWanSessionTestAccess::transport(session, binds, stops, dials);
        session.connectRadio(request(std::make_shared<Account>()));
        FlexWanSessionTestAccess::connectedForCleanup(session);
        session.commands().setWriter([&](const FlexCommandSession::Write& write) {
            const auto result = write.run([&] { writes.append(write.command); return FlexCommandSession::WriteResult::Written; });
            session.commands().writeFinished(write.epoch, write.sequence, result);
            QTimer::singleShot(0, &session, [&, write] {
                QCOMPARE(session.commands().submit(QStringLiteral("ordinary setting"), {}), quint32(0));
                session.commands().answer(write.epoch, write.sequence, 0, {});
            });
            return true;
        });
        QSignalSpy ended(&session, &FlexWanSession::disconnected);
        RadioDisconnectRequest close;
        close.receiveStreamId = 0x123;
        close.completion = deferred ? RadioDisconnectRequest::Completion::Deferred : RadioDisconnectRequest::Completion::Wait;
        session.disconnectRadio(close);
        QCOMPARE(ended.size(), deferred ? 0 : 1);
        QTRY_COMPARE(ended.size(), 1);
        QCOMPARE(writes, (QStringList{QStringLiteral("keepalive disable"), QStringLiteral("stream remove 0x123")}));
        QVERIFY(!session.active());
    }

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

    void headlessCancellationDuringReservationDoesNotReadCredentials()
    {
        RadioModel model;
        auto target = control::makeModelRadioConnectionTarget(&model);
        QVERIFY(target);
        int reads = 0;
        SmartLinkClient& client = SmartLinkAccountModelTestAccess::client(model.smartLinkAccounts());
        SmartLinkCredentialsTestAccess::execute(SmartLinkClientTestAccess::credentials(client),
            [&](auto, const QString&, const QString&, auto completion) { ++reads; completion(false, {}); });
        connect(target.get(), &control::RadioConnectionTarget::stateChanged, target.get(), [&] {
            if (target->state() == control::RadioConnectionTarget::State::Connecting) { target->disconnectRadio(); }
        });
        QVERIFY(target->connectConfiguredRemote(QStringLiteral("legacy"), QStringLiteral("selected")));
        QTRY_COMPARE(target->state(), control::RadioConnectionTarget::State::Idle);
        QCOMPARE(reads, 0);
        QVERIFY(!model.isConnectAttemptInFlight());
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

    void brokerHandleValidation_data()
    {
        QTest::addColumn<QString>("handle");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("auth0-handle") << QStringLiteral("auth0|0123456789abcdef01234567_00000000-1111-2222-3333-444444444444") << true;
        QTest::newRow("empty") << QString() << false;
        QTest::newRow("newline") << QStringLiteral("handle\n") << false;
        QTest::newRow("command-injection") << QStringLiteral("handle\r\nC1|transmit set rfpower=100") << false;
        QTest::newRow("tab") << QStringLiteral("handle\tother") << false;
        QTest::newRow("overlong") << QString(513, QLatin1Char('a')) << false;
    }

    void brokerHandleValidation()
    {
        QFETCH(QString, handle);
        QFETCH(bool, accepted);
        SmartLinkClient client;
        SmartLinkClientTestAccess::authenticated(client);
        QString received;
        int failures = 0;
        const quint64 lease = client.beginConnection(QStringLiteral("radio"), 49152, {},
            [&](quint64, const QString& value) { received = value; },
            [&](quint64, const QString&) { ++failures; });
        QVERIFY(lease);
        SmartLinkClientTestAccess::sent(client);
        SmartLinkClientTestAccess::reply(client, QStringLiteral("radio"), handle);
        QCOMPARE(received, accepted ? handle : QString());
        QCOMPARE(failures, accepted ? 0 : 1);
        client.cancelConnection(lease);
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
