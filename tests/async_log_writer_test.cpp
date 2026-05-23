#include "core/AsyncLogWriter.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QRegularExpression>
#include <QString>
#include <QTemporaryDir>
#include <QTime>

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) ++g_failed;
}

QByteArray readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

QString writeAndRead(const QString& path, QtMsgType type, const QString& category,
                     const QString& message)
{
    AsyncLogWriter w;
    if (!w.start(path, false)) {
        return {};
    }
    w.enqueue(type, QTime(12, 34, 56, 789), category, message);
    w.flush();
    w.shutdown();
    return QString::fromUtf8(readAll(path));
}

void testFormatPreservation(const QString& dir)
{
    const QString path = dir + "/format.log";
    const QString contents = writeAndRead(path, QtDebugMsg,
                                          QStringLiteral("aether.x"),
                                          QStringLiteral("hello"));
    // Expected: "[12:34:56.789] DBG aether.x: hello\n"
    const bool ok = contents == QStringLiteral("[12:34:56.789] DBG aether.x: hello\n");
    report("produced line matches [HH:mm:ss.zzz] LVL cat: msg shape", ok);
}

void testLabelForEachMsgType(const QString& dir)
{
    struct Case { QtMsgType type; const char* label; const char* name; };
    const Case cases[] = {
        { QtDebugMsg,    "DBG", "DBG label for QtDebugMsg" },
        { QtInfoMsg,     "INF", "INF label for QtInfoMsg" },
        { QtWarningMsg,  "WRN", "WRN label for QtWarningMsg" },
        { QtCriticalMsg, "CRT", "CRT label for QtCriticalMsg" },
    };
    for (const Case& c : cases) {
        const QString path = QString("%1/label_%2.log").arg(dir).arg(c.label);
        const QString contents = writeAndRead(path, c.type,
                                              QStringLiteral("aether.x"),
                                              QStringLiteral("payload"));
        const QString expected = QString("[12:34:56.789] %1 aether.x: payload\n").arg(c.label);
        report(c.name, contents == expected);
    }
}

void testIpv4Redaction(const QString& dir)
{
    const QString path = dir + "/ipv4.log";
    const QString contents = writeAndRead(path, QtDebugMsg,
                                          QStringLiteral("aether.x"),
                                          QStringLiteral("client 192.168.50.121 connected"));
    report("IPv4 redaction preserves last octet",
           contents.contains(QStringLiteral("*.*.*. 121"))
           && !contents.contains(QStringLiteral("192.168.50.121")));
}

void testIpv4VersionExemption(const QString& dir)
{
    const QString path = dir + "/ipv4_ver.log";
    const QString contents = writeAndRead(path, QtDebugMsg,
                                          QStringLiteral("aether.x"),
                                          QStringLiteral("software_ver=4.2.18.41174"));
    report("IPv4 redaction skips ver= firmware versions",
           contents.contains(QStringLiteral("software_ver=4.2.18.41174")));
}

void testIpv4ThreeOctetNotRedacted(const QString& dir)
{
    // Three-octet strings like "0.9.8" never match the IPv4 regex (which requires four octets).
    // This case in the original ticket was a misread of the regex; lock in current behavior.
    const QString path = dir + "/ipv4_three.log";
    const QString contents = writeAndRead(path, QtDebugMsg,
                                          QStringLiteral("aether.x"),
                                          QStringLiteral("version \"0.9.8\""));
    report("three-octet versions are not touched by IPv4 redaction",
           contents.contains(QStringLiteral("\"0.9.8\"")));
}

void testIpv4QuotedFourOctetIsStillRedacted(const QString& dir)
{
    // Quoting four-octet IPs does NOT exempt them — quoting is not authentication of intent.
    const QString path = dir + "/ipv4_quoted.log";
    const QString contents = writeAndRead(path, QtDebugMsg,
                                          QStringLiteral("aether.x"),
                                          QStringLiteral("peer \"10.0.0.5\""));
    report("quoted four-octet IPv4 is still redacted",
           contents.contains(QStringLiteral("*.*.*. 5"))
           && !contents.contains(QStringLiteral("10.0.0.5")));
}

void testSerialRedaction(const QString& dir)
{
    const QString path = dir + "/serial.log";
    const QString contents = writeAndRead(path, QtDebugMsg,
                                          QStringLiteral("aether.x"),
                                          QStringLiteral("radio serial 4424-1213-8600-7836"));
    report("radio serial redaction preserves last group",
           contents.contains(QStringLiteral("****-****-****-7836"))
           && !contents.contains(QStringLiteral("4424-1213-8600-7836")));
}

void testTokenRedaction(const QString& dir)
{
    struct Case {
        const char* name;
        QString input;
        QString mustContain;
        QString mustNotContain;
    };
    // The redactor keeps a 4-char prefix of the token for cross-line correlation
    // and substitutes "***REDACTED***" for the remainder. (#2954)
    const Case cases[] = {
        { "id_token= keyword scrubs tail, preserves prefix",
          QStringLiteral("auth id_token=ABCDEF12345678901234extra_payload_more"),
          QStringLiteral("id_token=ABCD***REDACTED***"),
          QStringLiteral("extra_payload_more") },
        { "ID_TOKEN= keyword matches case-insensitively",
          QStringLiteral("ID_TOKEN=ABCDEFGHIJKLMNOPQRSTextra_payload_more"),
          QStringLiteral("ID_TOKEN=ABCD***REDACTED***"),
          QStringLiteral("extra_payload_more") },
        { "access_token= keyword is covered",
          QStringLiteral("access_token=ABCDEFGHIJKLMNOPextra_payload_more"),
          QStringLiteral("access_token=ABCD***REDACTED***"),
          QStringLiteral("extra_payload_more") },
        { "refresh_token= keyword is covered",
          QStringLiteral("refresh_token=ABCDEFGHIJKLMNOPextra_payload_more"),
          QStringLiteral("refresh_token=ABCD***REDACTED***"),
          QStringLiteral("extra_payload_more") },
        { "auth= keyword is covered",
          QStringLiteral("auth=ABCDEFGHIJKLMNOPextra_payload_more"),
          QStringLiteral("auth=ABCD***REDACTED***"),
          QStringLiteral("extra_payload_more") },
        { "Authorization: header with no scheme",
          QStringLiteral("Authorization: ABCDEFGHIJKLMNOPextra_payload_more"),
          QStringLiteral("Authorization: ABCD***REDACTED***"),
          QStringLiteral("extra_payload_more") },
        { "Authorization: Bearer scheme preserves \"Bearer \"",
          QStringLiteral("Authorization: Bearer ABCDEFGHIJKLMNOPextra_payload_more"),
          QStringLiteral("Authorization: Bearer ABCD***REDACTED***"),
          QStringLiteral("extra_payload_more") },
        { "bare bearer scheme preserves \"bearer \"",
          QStringLiteral("got bearer ABCDEFGHIJKLMNOPextra_payload_more from header"),
          QStringLiteral("bearer ABCD***REDACTED***"),
          QStringLiteral("extra_payload_more") },
    };
    for (const Case& c : cases) {
        const QString path = QString("%1/token_%2.log").arg(dir).arg(QString::fromUtf8(c.name).left(20));
        const QString contents = writeAndRead(path, QtDebugMsg,
                                              QStringLiteral("aether.x"),
                                              c.input);
        report(c.name, contents.contains(c.mustContain)
                       && !contents.contains(c.mustNotContain));
    }
}

void testTokenFalsePositiveBoundary(const QString& dir)
{
    // The \b word boundary keeps app-specific identifiers that end in "token"
    // (e.g. "keytoken") from being scrubbed as if they were the token keyword. (#2954)
    const QString path = dir + "/token_boundary.log";
    const QString contents = writeAndRead(path, QtDebugMsg,
                                          QStringLiteral("aether.x"),
                                          QStringLiteral("loaded keytoken=fixture_value_unchanged"));
    report("\\b prevents \"keytoken=\" from being treated as a token keyword",
           contents.contains(QStringLiteral("keytoken=fixture_value_unchanged")));
}

void testMacDashRedaction(const QString& dir)
{
    const QString path = dir + "/mac_dash.log";
    const QString contents = writeAndRead(path, QtDebugMsg,
                                          QStringLiteral("aether.x"),
                                          QStringLiteral("mac 00-1C-2D-05-37-2A here"));
    report("MAC dash redaction preserves last octet",
           contents.contains(QStringLiteral("**-**-**-**-**-2A"))
           && !contents.contains(QStringLiteral("00-1C-2D-05-37-2A")));
}

void testMacColonRedaction(const QString& dir)
{
    const QString path = dir + "/mac_colon.log";
    const QString contents = writeAndRead(path, QtDebugMsg,
                                          QStringLiteral("aether.x"),
                                          QStringLiteral("mac 00:1C:2D:05:37:2A here"));
    report("MAC colon redaction preserves last octet",
           contents.contains(QStringLiteral("**:**:**:**:**:2A"))
           && !contents.contains(QStringLiteral("00:1C:2D:05:37:2A")));
}

void testClearLogTruncatesPriorButPreservesSubsequent(const QString& dir)
{
    const QString path = dir + "/clear.log";
    AsyncLogWriter w;
    if (!w.start(path, false)) {
        report("clearLog precondition: writer starts", false);
        return;
    }
    w.enqueue(QtDebugMsg, QTime(1, 0, 0, 0), QStringLiteral("aether.x"),
              QStringLiteral("before-clear-line"));
    w.flush();
    w.clearLog();
    w.enqueue(QtDebugMsg, QTime(1, 0, 0, 1), QStringLiteral("aether.x"),
              QStringLiteral("after-clear-line"));
    w.flush();
    const QString contents = QString::fromUtf8(readAll(path));
    w.shutdown();

    report("clearLog truncates lines enqueued before the clear",
           !contents.contains(QStringLiteral("before-clear-line")));
    report("clearLog preserves lines enqueued after the clear",
           contents.contains(QStringLiteral("after-clear-line")));
}

void testShutdownDrainsAllEnqueuedLines(const QString& dir)
{
    const QString path = dir + "/shutdown.log";
    AsyncLogWriter w;
    if (!w.start(path, false)) {
        report("shutdown drains: writer starts", false);
        return;
    }
    constexpr int kCount = 200;
    for (int i = 0; i < kCount; ++i) {
        w.enqueue(QtDebugMsg, QTime(0, 0, 0, i % 1000),
                  QStringLiteral("aether.x"),
                  QString("payload-%1").arg(i));
    }
    // No explicit flush — shutdown must drain remaining queue.
    w.shutdown();

    const QByteArray contents = readAll(path);
    int newlines = contents.count('\n');
    report("shutdown drains all enqueued lines (no loss on graceful stop)",
           newlines == kCount);
}

void testDropAccountingEmitsSummaryAndCounters(const QString& dir)
{
    const QString path = dir + "/drops.log";
    AsyncLogWriter w;
    if (!w.start(path, false)) {
        report("drop accounting: writer starts", false);
        return;
    }
    // Overshoot the hard cap (kHardMaxQueueEntries = 9216) substantially so
    // that even with worker draining we still observe drops in counters.
    constexpr int kBurst = 30000;
    for (int i = 0; i < kBurst; ++i) {
        w.enqueue(QtDebugMsg, QTime(0, 0, 0, i % 1000),
                  QStringLiteral("aether.x"),
                  QString("burst-%1").arg(i));
    }
    w.flush();
    const auto c = w.counters();
    w.shutdown();

    const QString contents = QString::fromUtf8(readAll(path));
    report("drop accounting: droppedDebugInfoLines counter advances",
           c.droppedDebugInfoLines > 0);
    report("drop accounting: summary line is written for dropped debug/info",
           contents.contains(QStringLiteral("Logging dropped debug/info lines count="))
           && contents.contains(QStringLiteral("aether.logging")));
}

void testHighPriorityReservePreservesCritical(const QString& dir)
{
    const QString path = dir + "/highprio.log";
    AsyncLogWriter w;
    if (!w.start(path, false)) {
        report("high-priority reserve: writer starts", false);
        return;
    }
    // Burst of debug to soak the queue, then a critical that must survive.
    constexpr int kDebugBurst = 12000;
    for (int i = 0; i < kDebugBurst; ++i) {
        w.enqueue(QtDebugMsg, QTime(0, 0, 0, i % 1000),
                  QStringLiteral("aether.x"),
                  QString("dbg-%1").arg(i));
    }
    w.enqueue(QtCriticalMsg, QTime(0, 0, 0, 0),
              QStringLiteral("aether.x"),
              QStringLiteral("MUST_SURVIVE_critical_marker"));
    w.flush();
    const auto c = w.counters();
    w.shutdown();

    const QString contents = QString::fromUtf8(readAll(path));
    report("high-priority reserve: critical line is preserved under debug burst",
           contents.contains(QStringLiteral("MUST_SURVIVE_critical_marker")));
    report("high-priority reserve: no warning/critical drops recorded",
           c.droppedHighPriorityLines == 0);
}

void testStderrMirroring(const QString& dir)
{
    const QString stderrPath = dir + "/captured_stderr.txt";
    const QString logPath = dir + "/mirror.log";

    // Redirect stderr to a file. freopen is portable C89.
    std::fflush(stderr);
    FILE* redirected = std::freopen(stderrPath.toUtf8().constData(), "w", stderr);
    if (!redirected) {
        report("stderr mirroring: freopen succeeds", false);
        return;
    }

    {
        AsyncLogWriter w;
        if (!w.start(logPath, true)) {
            report("stderr mirroring: writer starts", false);
        } else {
            w.enqueue(QtWarningMsg, QTime(8, 0, 0, 0),
                      QStringLiteral("aether.x"),
                      QStringLiteral("mirror-payload-1"));
            w.enqueue(QtCriticalMsg, QTime(8, 0, 0, 1),
                      QStringLiteral("aether.x"),
                      QStringLiteral("mirror-payload-2"));
            w.flush();
            w.shutdown();
        }
    }

    std::fflush(stderr);

    const QByteArray captured = readAll(stderrPath);
    const QByteArray fileBytes = readAll(logPath);

    report("stderr mirroring: captured stderr matches file contents",
           !fileBytes.isEmpty() && captured == fileBytes);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::printf("[FAIL] could not create temporary directory\n");
        return 1;
    }
    const QString dir = tmp.path();

    testFormatPreservation(dir);
    testLabelForEachMsgType(dir);
    testIpv4Redaction(dir);
    testIpv4VersionExemption(dir);
    testIpv4ThreeOctetNotRedacted(dir);
    testIpv4QuotedFourOctetIsStillRedacted(dir);
    testSerialRedaction(dir);
    testTokenRedaction(dir);
    testTokenFalsePositiveBoundary(dir);
    testMacDashRedaction(dir);
    testMacColonRedaction(dir);
    testClearLogTruncatesPriorButPreservesSubsequent(dir);
    testShutdownDrainsAllEnqueuedLines(dir);
    testDropAccountingEmitsSummaryAndCounters(dir);
    testHighPriorityReservePreservesCritical(dir);
    testStderrMirroring(dir);

    return g_failed == 0 ? 0 : 1;
}
