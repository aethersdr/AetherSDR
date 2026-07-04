// Regression tests for the CWX queue-drain watch state machine (#3949).
//
// These cover the three blocking correctness items from the PR #3979 review:
//   Item 3 — epoch/generation guard: a late cwx-send reply for a batch aborted
//            by ESC/clear/disconnect must not re-arm the watch; a disconnect
//            reset must let a fresh session arm at a *smaller* radio_index
//            (the monotonic guard must not wedge across reconnect).
//   Item 2 — live-mode sendChar arms the watch: a live-typing session must
//            advance and drain the watch (previously it armed nothing and the
//            ~60 s stuck-TX survived).
//   Item 1 — (CwxModel-side invariant) queueEmpty() fires purely from the armed
//            watch draining, independent of any interleaved status. This is the
//            guarantee the RadioModel flicker-immune release latch relies on;
//            the latch lifecycle itself lives in RadioModel wiring, which is not
//            unit-instantiable in this suite and is covered by the hardware test
//            plan (item 4) and code review.
//
// Run: ./build/cwx_drain_watch_test

#include "models/CwxModel.h"
#include <QCoreApplication>
#include <QSignalSpy>
#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-70s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

// Deliver a `cwx sent=<idx>` status, the mechanism that drains the watch.
void feedSent(CwxModel& m, int idx)
{
    m.applyStatus({{QStringLiteral("sent"), QString::number(idx)}});
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::printf("CWX drain-watch state-machine tests\n\n");

    // ── Item 3: stale reply after clear must not re-arm (epoch guard) ─────────
    {
        CwxModel m;
        const int epoch0 = m.drainEpoch();
        m.send("CQ");                       // emits replyCommandReady(cmd, epoch0)
        m.handleSendReply(0, "42,1", epoch0, 1); // reply arrives → arm at end 42 (1-char batch)
        report("item3: reply arms watch at radio_index",
               m.cwxEndIndex() == 42,
               "cwxEndIndex=" + std::to_string(m.cwxEndIndex()));

        m.clearBuffer();                    // ESC: resetDrainWatch bumps epoch, endIndex=-1
        report("item3: clearBuffer aborts watch",
               m.cwxEndIndex() == -1, "");
        report("item3: clearBuffer bumps epoch",
               m.drainEpoch() != epoch0, "");

        // Late reply for the discarded batch carries the pre-clear epoch.
        m.handleSendReply(0, "99,2", epoch0, 1);
        report("item3: stale (pre-clear epoch) reply is rejected, no re-arm",
               m.cwxEndIndex() == -1,
               "cwxEndIndex=" + std::to_string(m.cwxEndIndex()));
    }

    // ── Item 3: disconnect reset lets a new session arm at a smaller index ────
    {
        CwxModel m;
        const int epochA = m.drainEpoch();
        m.send("LONG MESSAGE");
        m.handleSendReply(0, "500,1", epochA, 1);   // armed high, mid-macro
        report("item3: armed at 500 before disconnect",
               m.cwxEndIndex() == 500, "");

        m.resetDrainWatch();                     // simulates onDisconnected()
        const int epochB = m.drainEpoch();
        report("item3: resetDrainWatch clears index + bumps epoch",
               m.cwxEndIndex() == -1 && epochB != epochA, "");

        // New session's first reply has a smaller radio_index than the stale 500.
        m.send("CQ");
        m.handleSendReply(0, "10,1", epochB, 1);
        report("item3: post-reconnect smaller index arms cleanly (guard not wedged)",
               m.cwxEndIndex() == 10,
               "cwxEndIndex=" + std::to_string(m.cwxEndIndex()));
    }

    // ── Item 3: monotonic advance-only still holds within a session ───────────
    {
        CwxModel m;
        const int e = m.drainEpoch();
        m.send("A"); m.handleSendReply(0, "50,1", e, 1);   // arm at end 50
        m.send("B"); m.handleSendReply(0, "30,2", e, 1);   // out-of-order smaller
        report("item3: smaller in-session reply does not retract the watch",
               m.cwxEndIndex() == 50,
               "cwxEndIndex=" + std::to_string(m.cwxEndIndex()));
    }

    // ── Item 2: live-mode sendChar arms and drains the watch ──────────────────
    {
        CwxModel m;
        QSignalSpy replySpy(&m, &CwxModel::replyCommandReady);
        QSignalSpy emptySpy(&m, &CwxModel::queueEmpty);
        m.setLive(true);

        const int e = m.drainEpoch();
        m.sendChar("A");
        report("item2: sendChar emits via reply path (not fire-and-forget)",
               replySpy.count() == 1,
               "replyCommandReady count=" + std::to_string(replySpy.count()));

        m.handleSendReply(0, "5,1", e, 1);   // single live char: end == radio_index
        report("item2: live char arms the watch",
               m.cwxEndIndex() == 5, "");

        feedSent(m, 5);
        report("item2: live char drains → queueEmpty fires",
               emptySpy.count() == 1,
               "queueEmpty count=" + std::to_string(emptySpy.count()));
        report("item2: watch cleared after drain",
               m.cwxEndIndex() == -1, "");
    }

    // ── Item 1 (CwxModel invariant): watch drains via sent= despite noise ─────
    {
        CwxModel m;
        QSignalSpy emptySpy(&m, &CwxModel::queueEmpty);
        const int e = m.drainEpoch();
        m.send("CQ");
        m.handleSendReply(0, "10,1", e, 1);      // arm at end 10

        // Interleaved unrelated status (as arrives during interlock flicker).
        m.applyStatus({{QStringLiteral("wpm"), QStringLiteral("25")}});
        feedSent(m, 7);                          // partial — not yet drained
        report("item1: queueEmpty not fired before sent reaches end index",
               emptySpy.count() == 0,
               "queueEmpty count=" + std::to_string(emptySpy.count()));

        feedSent(m, 10);                         // reaches the watched index
        report("item1: queueEmpty fires exactly when sent reaches end index",
               emptySpy.count() == 1,
               "queueEmpty count=" + std::to_string(emptySpy.count()));
    }

    // ── Item 4a: radio_index is the FIRST char; end = radio_index + nChars-1 ──
    // Reproduces the FLEX-6500 fw 4.2.20 observation: a 23-char send into a
    // queue at sent=48 replied radio_index=49 (first char), and sent= climbed
    // to 71 as it keyed. The watch must fire at 71, NOT at 49 — releasing at 49
    // would truncate the release ~22 chars early (the pre-fix bug Jeremy caught).
    {
        CwxModel m;
        QSignalSpy emptySpy(&m, &CwxModel::queueEmpty);
        const int e = m.drainEpoch();
        m.send("PARIS PARIS PARIS PARIS");          // 23 chars
        m.handleSendReply(0, "49,1", e, 23);        // radio_index=49 (first char)
        report("item4a: end index = radio_index + nChars - 1 (49+23-1)",
               m.cwxEndIndex() == 71,
               "cwxEndIndex=" + std::to_string(m.cwxEndIndex()));

        feedSent(m, 49);                            // first char keyed — NOT drained
        report("item4a: no release when sent reaches only the first char (49)",
               emptySpy.count() == 0,
               "queueEmpty count=" + std::to_string(emptySpy.count()));
        feedSent(m, 60);                            // mid-message — still not drained
        report("item4a: no release mid-message (60)",
               emptySpy.count() == 0, "");
        feedSent(m, 71);                            // last char keyed — drained
        report("item4a: release exactly when sent reaches the last char (71)",
               emptySpy.count() == 1,
               "queueEmpty count=" + std::to_string(emptySpy.count()));
    }

    // ── Guard: sent= while idle (no armed watch) must not fire queueEmpty ─────
    {
        CwxModel m;
        QSignalSpy emptySpy(&m, &CwxModel::queueEmpty);
        feedSent(m, 100);                        // no watch armed
        report("idle: sent= with no armed watch does not fire queueEmpty",
               emptySpy.count() == 0,
               "queueEmpty count=" + std::to_string(emptySpy.count()));
    }

    // ── Cleanup: saveMacro strips client-only +/- before the shared radio store ─
    {
        CwxModel m;
        QSignalSpy cmdSpy(&m, &CwxModel::commandReady);
        m.saveMacro(0, "+CQ TEST");   // F1, raw text carries a speed modifier
        report("saveMacro keeps the raw +/- text client-side (for our expansion)",
               m.macro(0) == QStringLiteral("+CQ TEST"),
               "macro(0)=" + m.macro(0).toStdString());
        // radio-side command must have the '+' stripped and space -> 0x7f
        const QString expected =
            QStringLiteral("cwx macro save 1 \"CQ%1TEST\"").arg(QChar(0x7f));
        const QString got = cmdSpy.isEmpty() ? QString() : cmdSpy.last().at(0).toString();
        report("saveMacro strips +/- from the shared `cwx macro save` payload",
               got == expected,
               "got=" + got.toStdString());
    }

    // ── Cleanup: sendMacro falls back to radio-side expansion when unsynced ────
    {
        CwxModel m;
        QSignalSpy cmdSpy(&m, &CwxModel::commandReady);
        m.sendMacro(1);   // F1 not yet synced (m_macros[0] empty) — must not no-op
        const QString got = cmdSpy.isEmpty() ? QString() : cmdSpy.last().at(0).toString();
        report("sendMacro (unsynced) falls back to `cwx macro send N`, not silence",
               got == QStringLiteral("cwx macro send 1"),
               "got=" + got.toStdString());
    }

    std::printf("\n%s\n", g_failed == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return g_failed == 0 ? 0 : 1;
}
