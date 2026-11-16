#include "core/backends/flex/FlexCommandSession.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/WanConnection.h"
#include "TestSettingsProfile.h"

#include <QtTest>
#include <limits>
#include <memory>

using namespace AetherSDR;
using Session = FlexCommandSession;

namespace AetherSDR {
class FlexCommandSessionTestAccess {
public:
    static void nearExhaustion(Session& session)
    { session.m_sequence.store(std::numeric_limits<quint32>::max()); }
};
class WanConnectionTestAccess {
public:
    static void begin(WanConnection& wan) { wan.resetSessionState(); }
    static void remoteClose(WanConnection& wan) { wan.onTlsDisconnected(); }
};
}

// Exercises production queue/reply ownership with inert writes. No RadioModel,
// network endpoint, listening socket or synthetic firmware peer is involved.
class FlexCommandSessionTest : public QObject {
    Q_OBJECT
private slots:
    void wanRetirementKeepsReentrantReplacement()
    {
        WanConnection wan;
        WanConnectionTestAccess::begin(wan);
        wan.commands().setWriter([](const Session::Write&) { return true; });
        int disconnected = 0;
        connect(&wan, &WanConnection::disconnected, &wan, [&] { ++disconnected; });
        QVERIFY(wan.commands().submit("old", {}, [&](const Session::Reply& reply) {
            QCOMPARE(reply.kind, Session::ReplyKind::NotDispatched);
            WanConnectionTestAccess::begin(wan);
            QVERIFY(wan.commands().submit("replacement", {}));
        }));
        WanConnectionTestAccess::remoteClose(wan);
        QCOMPARE(wan.commandSessionGeneration(), quint64(3));
        QCOMPARE(wan.commands().epoch(), quint64(3));
        QCOMPARE(wan.commands().pendingCount(), 1);
        QCOMPARE(disconnected, 0);
    }

    void wanRetirementCanDestroyOwner()
    {
        auto wan = std::make_unique<WanConnection>();
        WanConnectionTestAccess::begin(*wan);
        wan->commands().setWriter([](const Session::Write&) { return true; });
        QVERIFY(wan->commands().submit("old", {}, [&](const Session::Reply&) { wan.reset(); }));
        wan->disconnectFromRadio();
        QVERIFY(!wan);
    }

    void typedBackendDispatchNeedsNoRadioModel()
    {
        FlexBackend backend;
        Session session;
        session.begin(1);
        backend.setCommandSessionForTest(&session);
        QList<Session::Write> writes;
        session.setWriter([&](const Session::Write& write) { writes.append(write); return true; });
        int completed = 0;
        QCOMPARE(backend.requestSessionControl({SessionControlField::MicrophoneInputs},
            SessionCommandGuard::create(), [&](const SessionControlReply& reply) {
                QVERIFY(reply.accepted());
                QCOMPARE(reply.microphones, QStringList({"MIC", "BAL"}));
                ++completed;
            }), ReceiveDispatch::Dispatched);
        QCOMPARE(backend.requestRadioSetup({RadioSetupField::ShowTxInWaterfall, true},
            [&](const RadioControlReply& reply) { QCOMPARE(reply.kind, RadioControlReply::Kind::Accepted); ++completed; }),
            ReceiveDispatch::Dispatched);
        QCOMPARE(writes.size(), 2);
        QCOMPARE(writes[0].command, "mic list");
        QVERIFY(writes[1].sequence > writes[0].sequence);
        for (const auto& write : writes) {
            session.writeFinished(write.epoch, write.sequence, write.run([] { return Session::WriteResult::Written; }));
            session.answer(write.epoch, write.sequence, 0, write.command == "mic list" ? "MIC,BAL" : "");
        }
        QCOMPARE(completed, 2);
        QCOMPARE(session.pendingCount(), 0);
    }

    void orderedCleanupSurvivesAdmissionCloseButNotRetirement()
    {
        Session session;
        session.begin(1);
        QList<Session::Write> queued;
        session.setWriter([&](const Session::Write& write) { queued.append(write); return true; });
        QVERIFY(session.submit("ordinary", {}));
        Session::Options cleanup;
        cleanup.cleanup = true;
        QVERIFY(session.submit("restore", cleanup));
        QVERIFY(session.submit("late restore", cleanup));
        session.invalidate();
        const auto write = [] { return Session::WriteResult::Written; };
        QCOMPARE(queued[0].run(write), Session::WriteResult::NotWritten);
        QCOMPARE(queued[1].run(write), Session::WriteResult::Written);
        session.retire("drained");
        QCOMPARE(queued[2].run(write), Session::WriteResult::NotWritten);
    }

    void sessionReplyDistinguishesRefusalFromUnknown()
    {
        FlexBackend backend;
        Session session;
        backend.setCommandSessionForTest(&session);
        session.begin(1);
        std::optional<Session::Write> queued;
        session.setWriter([&](const Session::Write& write) { queued = write; return true; });
        RadioControlReply::Kind result = RadioControlReply::Kind::Accepted;
        const auto request = [&] {
            return backend.requestSessionControl({SessionControlField::RegisterGui},
                SessionCommandGuard::create(), [&](const SessionControlReply& reply) { result = reply.result.kind; });
        };
        QCOMPARE(request(), ReceiveDispatch::Dispatched);
        session.writeFinished(queued->epoch, queued->sequence, queued->run([] { return Session::WriteResult::Written; }));
        session.answer(queued->epoch, queued->sequence, 17, "slots occupied");
        QCOMPARE(result, RadioControlReply::Kind::Refused);
        QCOMPARE(request(), ReceiveDispatch::Dispatched);
        session.writeFinished(queued->epoch, queued->sequence, queued->run([] { return Session::WriteResult::Unknown; }));
        QCOMPARE(result, RadioControlReply::Kind::Failed);
        QCOMPARE(request(), ReceiveDispatch::Dispatched);
        session.retire("cancelled before write");
        QCOMPARE(result, RadioControlReply::Kind::Cancelled);
    }

    void partialWriteAndRetiredReplyAreNotReportedAsUnwritten()
    {
        Session session;
        session.begin(1);
        session.setWriter([&](const Session::Write& write) {
            const auto result = write.run([] { return Session::WriteResult::Unknown; });
            session.writeFinished(write.epoch, write.sequence, result);
            return true;
        });
        int completed = 0;
        QVERIFY(session.submit("partial", {}, [&](const Session::Reply& reply) {
            QCOMPARE(reply.kind, Session::ReplyKind::Unknown);
            ++completed;
        }));
        session.setWriter([&](const Session::Write& write) {
            write.run([] { return Session::WriteResult::Written; });
            session.answer(write.epoch, write.sequence, 17, "refused");
            return true; // The receipt is still queued when disconnect arrives.
        });
        QVERIFY(session.submit("reply before close", {}, [&](const Session::Reply& reply) {
            QCOMPARE(reply.kind, Session::ReplyKind::RadioReply);
            QCOMPARE(reply.code, 17);
            ++completed;
        }));
        session.retire("closed");
        QCOMPARE(completed, 2);
    }

    void immediateReplyWaitsForWriteReceipt()
    {
        Session session;
        session.begin(1);
        QStringList events;
        bool prematureCompletion = false;
        Session::Options options;
        options.written = [&](Session::WriteResult) { events.append("written"); };
        options.consumed = [&] { events.append("consumed"); };
        session.setWriter([&](const Session::Write& write) {
            const auto result = write.run([&] {
                session.answer(write.epoch, write.sequence, 0, "answer");
                prematureCompletion = !events.isEmpty();
                return Session::WriteResult::Written;
            });
            session.writeFinished(write.epoch, write.sequence, result);
            return true;
        });
        QVERIFY(session.submit("ping", options, [&](const Session::Reply& reply) {
            QCOMPARE(reply.kind, Session::ReplyKind::RadioReply);
            QCOMPARE(reply.body, "answer");
            events.append("reply");
        }));
        QCOMPARE(events, QStringList({"written", "consumed", "reply"}));
        QVERIFY(!prematureCompletion);
        QCOMPARE(session.pendingCount(), 0);
    }

    void oldSessionCannotAnswerOrWriteIntoReplacement()
    {
        Session session;
        QList<Session::Write> writes;
        session.setWriter([&](const Session::Write& write) { writes.append(write); return true; });
        session.begin(10);
        int canceled = 0;
        QVERIFY(session.submit("first", {}, [&](const Session::Reply& reply) {
            QCOMPARE(reply.kind, Session::ReplyKind::NotDispatched);
            ++canceled;
        }));
        session.begin(11);
        QCOMPARE(canceled, 1);
        int answers = 0;
        const quint32 sequence = session.submit("replacement", {}, [&](const Session::Reply&) { ++answers; });
        QCOMPARE(sequence, writes.first().sequence);
        bool oldWritten = false;
        QCOMPARE(writes.first().run([&] { oldWritten = true; return Session::WriteResult::Written; }),
                 Session::WriteResult::NotWritten);
        QVERIFY(!oldWritten);
        session.answer(10, sequence, 0, "stale");
        session.writeFinished(10, sequence, Session::WriteResult::Written);
        QCOMPARE(answers, 0);
        session.writeFinished(11, sequence, writes.last().run([] { return Session::WriteResult::Written; }));
        session.answer(11, sequence, 0, "current");
        session.answer(11, sequence, 0, "duplicate");
        QCOMPARE(answers, 1);
    }

    void timeoutCancelsQueuedWrite()
    {
        Session session;
        std::optional<Session::Write> queued;
        session.setWriter([&](const Session::Write& write) { queued = write; return true; });
        session.begin(1);
        Session::Options options;
        options.timeoutMs = 1;
        int completions = 0;
        QVERIFY(session.submit("late", options, [&](const Session::Reply& reply) {
            QCOMPARE(reply.kind, Session::ReplyKind::NotDispatched);
            ++completions;
        }));
        QTRY_COMPARE(completions, 1);
        bool written = false;
        QCOMPARE(queued->run([&] { written = true; return Session::WriteResult::Written; }),
                 Session::WriteResult::NotWritten);
        QVERIFY(!written);
        session.answer(1, queued->sequence, 0, {});
        QCOMPARE(completions, 1);
    }

    void timeoutAfterWriteIsUnknown()
    {
        Session session;
        session.begin(1);
        session.setWriter([&](const Session::Write& write) {
            session.writeFinished(write.epoch, write.sequence,
                write.run([] { return Session::WriteResult::Written; }));
            return true;
        });
        Session::Options options;
        options.timeoutMs = 1;
        int completions = 0;
        QVERIFY(session.submit("request", options, [&](const Session::Reply& reply) {
            QCOMPARE(reply.kind, Session::ReplyKind::Unknown);
            ++completions;
        }));
        QTRY_COMPARE(completions, 1);
        QCOMPARE(session.pendingCount(), 0);
    }

    void retirementCanStartReplacement()
    {
        Session session;
        session.begin(1);
        session.setWriter([](const Session::Write&) { return true; });
        QVERIFY(session.submit("old", {}, [&](const Session::Reply&) {
            session.begin(3);
            QVERIFY(session.submit("new", {}));
        }));
        session.begin(2);
        QCOMPARE(session.epoch(), 3);
        QCOMPARE(session.pendingCount(), 1);
    }

    void callbackCanDestroyOwner()
    {
        auto session = std::make_unique<Session>();
        session->begin(1);
        session->setWriter([](const Session::Write&) { return true; });
        QVERIFY(session->submit("destroy", {}, [&](const Session::Reply&) { session.reset(); }));
        session->retire("end");
        QVERIFY(!session);
    }

    void cleanupRemainsAdmittedAtOrdinaryCapacity()
    {
        Session session;
        session.begin(1);
        session.setWriter([](const Session::Write&) { return true; });
        for (qsizetype i = 0; i < Session::kMaximumPending; ++i) { QVERIFY(session.submit("ordinary", {})); }
        QCOMPARE(session.submit("overflow", {}), 0);
        Session::Options cleanup;
        cleanup.cleanup = true;
        QVERIFY(session.submit("cleanup", cleanup));
        session.retire("end");
        QCOMPARE(session.pendingCount(), 0);
    }

    void sequenceExhaustionNeverReusesAnId()
    {
        Session session;
        session.begin(1);
        FlexCommandSessionTestAccess::nearExhaustion(session);
        QCOMPARE(session.allocateSequence(), std::numeric_limits<quint32>::max());
        QCOMPARE(session.allocateSequence(), 0);
        QCOMPARE(session.allocateSequence(), 0);
        session.begin(2);
        QCOMPARE(session.allocateSequence(), 1);
    }

    void canceledGuardAndMissingTxAuthorityNeverWrite()
    {
        Session session;
        session.begin(1);
        std::optional<Session::Write> queued;
        session.setWriter([&](const Session::Write& write) { queued = write; return true; });
        Session::Options options;
        options.guard = SessionCommandGuard::create();
        options.guarded = true;
        QVERIFY(session.submit("guarded", options));
        options.guard.cancel();
        bool written = false;
        const auto writer = [&] { written = true; return Session::WriteResult::Written; };
        QCOMPARE(queued->run(writer), Session::WriteResult::NotWritten);
        options = {};
        options.tx = true;
        options.keying = true;
        QVERIFY(session.submit("key", options));
        QCOMPARE(queued->run(writer), Session::WriteResult::NotWritten);
        QVERIFY(!written);
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("flex-command-session"));
    QCoreApplication app(argc, argv);
    FlexCommandSessionTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "flex_command_session_test.moc"
