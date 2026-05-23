// Standalone test harness for NavtexModel state machine.
//
// Build: produced by CMake as `navtex_model_test`.
// Run:   ./build/navtex_model_test
// Exit:  0 = pass, 1 = fail.

#include "models/NavtexModel.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <cstdio>
#include <cstdlib>

using MasterSDR::NavtexModel;
using MasterSDR::NavtexMsg;
using MasterSDR::NavtexMsgStatus;
using MasterSDR::NavtexStatus;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const QString& detail = {}) {
    std::printf("%s %-58s %s\n", ok ? "[ OK ]" : "[FAIL]", name,
                detail.toUtf8().constData());
    if (!ok) ++g_failed;
}

QString lastCommand(QSignalSpy& spy) {
    if (spy.isEmpty()) return {};
    const auto args = spy.last();
    if (args.isEmpty()) return {};
    return args.first().toString();
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    // ── Pending → Queued (happy path via response) ─────────────────────────
    {
        NavtexModel m;
        QSignalSpy cmdSpy(&m, &NavtexModel::replyCommandReady);
        QSignalSpy msgsSpy(&m, &NavtexModel::messagesChanged);

        m.sendMessage('A', 'B', "TEST MESSAGE");

        report("sendMessage emits replyCommandReady",
               cmdSpy.size() == 1);

        const QString cmd = cmdSpy.last().first().toString();
        const int seq = cmdSpy.last().last().toInt();
        report("command starts with 'navtex send'",
               cmd.startsWith("navtex send"));
        report("command includes tx_ident=A",
               cmd.contains("tx_ident=A"));
        report("command includes subject_indicator=B",
               cmd.contains("subject_indicator=B"));
        report("command quotes msg_text",
               cmd.contains("msg_text=\"TEST MESSAGE\""));

        // Radio confirms with index 42
        m.handleSendResponse(seq, 0, "42");

        report("handleSendResponse(success) → message added",
               m.messages().size() == 1);
        report("message status promoted to Queued",
               m.messages().first().status == NavtexMsgStatus::Queued,
               QString::number(static_cast<int>(m.messages().first().status)));
        report("message idx = 42",
               m.messages().first().idx == 42u);
        report("messagesChanged emitted",
               msgsSpy.size() == 1);
    }

    // ── Queued → Sent via "navtex sent" status ─────────────────────────────
    {
        NavtexModel m;
        QSignalSpy cmdSpy(&m, &NavtexModel::replyCommandReady);
        m.sendMessage('A', 'B', "MSG");
        const int seq = cmdSpy.last().last().toInt();
        m.handleSendResponse(seq, 0, "7");

        QMap<QString, QString> sentKvs;
        sentKvs["idx"] = "7";
        sentKvs["serial_num"] = "100";
        m.parseStatus("navtex sent", sentKvs);

        report("status promoted to Sent",
               m.messages().first().status == NavtexMsgStatus::Sent);
        report("serial_num populated",
               m.messages().first().serialNum == 100u);
        report("dateTime stamped",
               !m.messages().first().dateTime.isEmpty());
    }

    // ── Send failure (resp != 0) ───────────────────────────────────────────
    {
        NavtexModel m;
        QSignalSpy cmdSpy(&m, &NavtexModel::replyCommandReady);
        m.sendMessage('A', 'B', "FAIL");
        const int seq = cmdSpy.last().last().toInt();
        m.handleSendResponse(seq, /*respVal=*/0x50001000, "");

        report("send failure → message added with Error status",
               m.messages().size() == 1
            && m.messages().first().status == NavtexMsgStatus::Error);
    }

    // ── Orphan "navtex sent" for unknown idx (Multi-Flex case) ─────────────
    {
        NavtexModel m;
        QMap<QString, QString> sentKvs;
        sentKvs["idx"] = "999";
        sentKvs["serial_num"] = "55";
        m.parseStatus("navtex sent", sentKvs);

        report("orphan navtex sent synthesized into messages",
               m.messages().size() == 1
            && m.messages().first().idx == 999u
            && m.messages().first().status == NavtexMsgStatus::Sent);
    }

    // ── Status string parsing (case-insensitive per FlexLib) ───────────────
    {
        NavtexModel m;
        QSignalSpy statusSpy(&m, &NavtexModel::statusChanged);

        QMap<QString, QString> kvs;
        kvs["status"] = "Active";
        m.parseStatus("navtex", kvs);
        report("status=Active recognised",
               m.status() == NavtexStatus::Active && statusSpy.size() == 1);

        kvs["status"] = "TRANSMITTING";  // uppercase
        m.parseStatus("navtex", kvs);
        report("status=TRANSMITTING (uppercase) recognised",
               m.status() == NavtexStatus::Transmitting);

        kvs["status"] = "queuefull";  // lowercase
        m.parseStatus("navtex", kvs);
        report("status=queuefull (lowercase) recognised",
               m.status() == NavtexStatus::QueueFull);

        kvs["status"] = "garbage";
        m.parseStatus("navtex", kvs);
        report("unknown status falls back to Error",
               m.status() == NavtexStatus::Error);
    }

    // ── msg_text quote escaping (the bug we fixed) ─────────────────────────
    {
        NavtexModel m;
        QSignalSpy cmdSpy(&m, &NavtexModel::replyCommandReady);
        m.sendMessage('A', 'B', "say \"hello\"");
        const QString cmd = cmdSpy.last().first().toString();

        report("embedded quote escaped as \\\"",
               cmd.contains("msg_text=\"say \\\"hello\\\"\""),
               cmd);

        m.sendMessage('A', 'B', "back\\slash");
        const QString cmd2 = cmdSpy.last().first().toString();
        report("embedded backslash escaped as \\\\",
               cmd2.contains("msg_text=\"back\\\\slash\""),
               cmd2);
    }

    // ── Idempotent navtex sent (already-Sent message) ──────────────────────
    {
        NavtexModel m;
        QSignalSpy cmdSpy(&m, &NavtexModel::replyCommandReady);
        m.sendMessage('A', 'B', "MSG");
        const int seq = cmdSpy.last().last().toInt();
        m.handleSendResponse(seq, 0, "5");

        QMap<QString, QString> sentKvs;
        sentKvs["idx"] = "5";
        m.parseStatus("navtex sent", sentKvs);
        const auto firstStamp = m.messages().first().dateTime;

        // Second occurrence shouldn't change state or duplicate
        m.parseStatus("navtex sent", sentKvs);
        report("redundant navtex sent ignored",
               m.messages().size() == 1
            && m.messages().first().dateTime == firstStamp);
    }

    if (g_failed == 0)
        std::printf("\nAll NavtexModel tests passed.\n");
    else
        std::printf("\n%d test(s) failed.\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
