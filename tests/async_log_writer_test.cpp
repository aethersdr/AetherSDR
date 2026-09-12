#include "core/AsyncLogWriter.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
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
    // Read back with QIODevice::Text to mirror how AsyncLogWriter WRITES the
    // file (it opens with QIODevice::Text so log files get native line endings).
    // Without the matching flag this read is asymmetric: on Windows the writer
    // emits "\r\n" while every expectation in this file is written as "\n", so
    // each exact-match assertion failed on the platform's line-ending
    // translation rather than on anything the writer got wrong. Text mode here
    // normalises "\r\n" back to "\n" on read, making the comparisons
    // line-ending agnostic on Windows and unchanged on POSIX.
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
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

void testIpv4QuotedVersionFieldExemption(const QString& dir)
{
    // The TCI client identity line (#5087) spells its field version="…"; a
    // 4-part authored ProductVersion must reach the bundle intact while the
    // peer address on the same line is still masked.
    const QString path = dir + "/ipv4_version_field.log";
    const QString contents = writeAndRead(path, QtDebugMsg,
                                          QStringLiteral("aether.x"),
                                          QStringLiteral("TciServer: client 127.0.0.1:61576 "
                                                         "process=\"JTDX\" version=\"2.2.159.0\""));
    report("IPv4 redaction skips version=\"…\" client versions",
           contents.contains(QStringLiteral("version=\"2.2.159.0\""))
           && contents.contains(QStringLiteral("*.*.*. 1:61576"))
           && !contents.contains(QStringLiteral("127.0.0.1")));
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

void testPersonalNameRedaction(const QString& dir)
{
    const QString path = dir + "/personal_names.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.smartlink"),
        QStringLiteral("application user_settings first_name=Pat last_name=Jensen "
                       "fullName='Pat Jensen' callsign=KK7GWY"));
    report("SmartLink personal names are absent from disk logs",
           !contents.contains(QStringLiteral("Pat"))
           && !contents.contains(QStringLiteral("Jensen"))
           && contents.count(QStringLiteral("***REDACTED***")) == 3
           && contents.contains(QStringLiteral("callsign=KK7GWY")));
}

void testCoordinateRedaction(const QString& dir)
{
    const QString path = dir + "/coordinates.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.connection"),
        QStringLiteral("gps lat=47.6205#lon=-122.3493 latitude:47.6205 "
                       "gps_longitude=-122.3493 location=47.6205,-122.3493"));
    report("GPS and location coordinates are absent from disk logs",
           !contents.contains(QStringLiteral("47.6205"))
           && !contents.contains(QStringLiteral("-122.3493"))
           && contents.count(QStringLiteral("***REDACTED***")) == 5);
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

void testRotationReopenFailureMirrorsToStderr(const QString& dir)
{
    auto runCase = [&](const QString& caseName, bool returnInvalidNewPath) {
        const QString stderrPath = dir + "/rotation_" + caseName + "_stderr.txt";
        const QString activeDir = dir + "/rotation_" + caseName + "_active";
        const QString logPath = activeDir + "/active.log";
        const QString marker = "rotation-fallback-" + caseName;

        QDir().mkpath(activeDir);
        std::fflush(stderr);
        FILE* redirected = std::freopen(stderrPath.toUtf8().constData(), "w", stderr);
        if (!redirected) {
            const QByteArray label = ("rotation fallback " + caseName
                                      + ": freopen succeeds").toUtf8();
            report(label.constData(), false);
            return;
        }

        bool blockerCreated = false;
        AsyncLogWriter w;
        w.setRotationConfig(1,
            [activeDir, returnInvalidNewPath, &blockerCreated](const QString& currentPath) {
                QFile::remove(currentPath);
                QDir(activeDir).removeRecursively();

                QFile blocker(activeDir);
                blockerCreated = blocker.open(QIODevice::WriteOnly | QIODevice::Truncate);
                blocker.close();

                if (returnInvalidNewPath) {
                    return activeDir + "/rotated.log";
                }
                return QString{};
            });

        const bool started = w.start(logPath, false);
        if (started) {
            w.enqueue(QtWarningMsg, QTime(9, 0, 0, 0),
                      QStringLiteral("aether.x"), QStringLiteral("trigger-rotation"));
            w.flush();
            w.enqueue(QtWarningMsg, QTime(9, 0, 0, 1),
                      QStringLiteral("aether.x"), marker);
            w.flush();
            w.shutdown();
        }

        std::fflush(stderr);
        const QByteArray captured = readAll(stderrPath);
        const QByteArray label = ("rotation fallback " + caseName
                                  + ": future logs mirror to stderr").toUtf8();
        report(label.constData(), started && blockerCreated
               && captured.contains(marker.toUtf8()));

        QFile::remove(activeDir);
    };

    runCase(QStringLiteral("same-path"), false);
    runCase(QStringLiteral("new-path"), true);
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


// ---------------------------------------------------------------------------
// #5480: coverage brought in line with current log sites. Every case below
// goes through the real writer (enqueue -> formatLine -> redactPii -> disk),
// not through redactPii() alone, so a rule that works in isolation but is
// bypassed on the way to the file still fails here.
// ---------------------------------------------------------------------------

void testHomePathRedaction(const QString& dir)
{
    const QString path = dir + "/home_paths.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.app"),
        QStringLiteral(R"(exe=/home/pat/AetherSDR/build/AetherSDR )"
                       R"(mac=/Users/pat.jensen/Applications/AetherSDR.app )"
                       R"(win=C:\Users\Pat Jensen\AppData\Local\AetherSDR\AetherSDR.exe )"
                       R"(qt=C:\\Users\\Pat Jensen\\AppData)"));
    report("home directory prefixes are replaced by ~ on all three platforms",
           !contents.contains(QStringLiteral("/home/pat"))
           && !contents.contains(QStringLiteral("/Users/pat.jensen"))
           && !contents.contains(QStringLiteral("Pat Jensen"))
           && contents.contains(QStringLiteral("~/AetherSDR/build"))
           && contents.contains(QStringLiteral("~/Applications"))
           && contents.contains(QStringLiteral("AppData")));
}

void testIpv6Redaction(const QString& dir)
{
    const QString path = dir + "/ipv6.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.connection"),
        QStringLiteral("full 2001:0db8:85a3:0000:0000:8a2e:0370:7334 "
                       "compressed 2001:db8::8a2e:370:7334 "
                       "bracketed [2001:db8::1]:4992 "
                       "mapped ::ffff:192.168.50.121 "
                       "scoped fe80::1%eth0"));
    report("IPv6 literals are redacted in every spelling",
           !contents.contains(QStringLiteral("8a2e"))
           && !contents.contains(QStringLiteral("2001:db8"))
           && !contents.contains(QStringLiteral("192.168.50.121"))
           && !contents.contains(QStringLiteral("fe80"))
           && contents.count(QStringLiteral("[v6-redacted]")) == 5);
}

void testIpv6DoesNotEatMacOrClock(const QString& dir)
{
    const QString path = dir + "/ipv6_boundary.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.connection"),
        QStringLiteral("mac 00:1C:2D:05:37:2A elapsed 12:34:56"));
    report("IPv6 rule does not consume MAC addresses or clock values",
           contents.contains(QStringLiteral("**:**:**:**:**:2A"))
           && contents.contains(QStringLiteral("12:34:56"))
           && !contents.contains(QStringLiteral("[v6-redacted]")));
}

void testEmailRedaction(const QString& dir)
{
    const QString path = dir + "/email.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.wan"),
        QStringLiteral("SmartLink login for pat.jensen@example.co.uk ok "
                       "email=other+tag@sub.example.com"));
    report("email addresses are redacted in prose and as field values",
           !contents.contains(QStringLiteral("pat.jensen@"))
           && !contents.contains(QStringLiteral("other+tag@"))
           && !contents.contains(QStringLiteral("example.co.uk")));
}

void testUserFieldRedaction(const QString& dir)
{
    const QString path = dir + "/user.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.icom"),
        // The whitespace form is QUOTED, because the real site
        // (IcomSession.cpp:273) streams a QString and QDebug quotes it. An
        // unquoted rule here would eat ordinary prose - see
        // testUserRuleTakesQuotedNamesOnly.
        QStringLiteral("control login user \"pat_jensen\" accepted username=pat.j "
                       "user_name=\"Pat\""));
    report("user name fields are redacted in keyword and quoted forms",
           !contents.contains(QStringLiteral("pat_jensen"))
           && !contents.contains(QStringLiteral("pat.j"))
           && !contents.contains(QStringLiteral("\"Pat\"")));
}

void testHostContextRedaction(const QString& dir)
{
    const QString path = dir + "/hosts.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.connection"),
        QStringLiteral("connecting to shack-pi.local:1883 ; "
                       "reconnecting to mqtt.home.arpa ; "
                       "disconnected from rotator.lan ; "
                       "connect to greenheron.lan:4533 timed out"));
    report("peer hostnames after a connection keyword are redacted",
           !contents.contains(QStringLiteral("shack-pi"))
           && !contents.contains(QStringLiteral("home.arpa"))
           && !contents.contains(QStringLiteral("rotator.lan"))
           && !contents.contains(QStringLiteral("greenheron"))
           // the diagnostic tail must survive
           && contents.contains(QStringLiteral(":1883"))
           && contents.contains(QStringLiteral("timed out")));
}

void testGridRedaction(const QString& dir)
{
    const QString path = dir + "/grid.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.freedv"),
        QStringLiteral("reported grid CN87ut peer grid_square=DM79lm "
                       "gridSquare:\"EM12ab\" locator FN31pr "
                       "location=\"47.6205,-122.3493\""));
    report("Maidenhead grids and quoted coordinate pairs are redacted",
           !contents.contains(QStringLiteral("CN87ut"))
           && !contents.contains(QStringLiteral("DM79lm"))
           && !contents.contains(QStringLiteral("EM12ab"))
           && !contents.contains(QStringLiteral("FN31pr"))
           && !contents.contains(QStringLiteral("47.6205")));
}

void testQuotedAndQtEscapedValueForms(const QString& dir)
{
    const QString path = dir + "/json_forms.log";
    // Build the escaped spelling the same way QDebug does for a QByteArray,
    // rather than hand-writing it: a hand-written fixture would not prove the
    // rule survives Qt's own quoting. (Ozy311 audit on #5480.)
    QString qtFormatted;
    QDebug(&qtFormatted) << QByteArray(
        R"({"access_token":"AuditJsonSecret","first_name":"Pat","latitude":47.6205})");
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.mqtt"),
        QStringLiteral(R"(plain {"access_token":"PlainJsonSecret"} qt )") + qtFormatted);
    report("quoted, JSON and Qt-escaped value spellings are all covered",
           !contents.contains(QStringLiteral("PlainJsonSecret"))
           && !contents.contains(QStringLiteral("AuditJsonSecret"))
           && !contents.contains(QStringLiteral("Pat"))
           && !contents.contains(QStringLiteral("47.6205")));
}

void testShortValueIsFullyRedacted(const QString& dir)
{
    const QString path = dir + "/short_values.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.wan"),
        QStringLiteral("a token=ab b token=abcd c token=abcdefgh"));
    // Assert the EXACT output, not merely the absence of the value followed by
    // a space: "token=ab***REDACTED***" contains neither "token=ab " nor a
    // bare "ab" boundary, so a laxer assertion here passes even when the
    // whole short value is retained as its own "prefix".
    report("a value no longer than the retained prefix is redacted whole",
           contents.contains(QStringLiteral("a token=***REDACTED*** b"))
           && contents.contains(QStringLiteral("b token=***REDACTED*** c"))
           && contents.contains(QStringLiteral("c token=abcd***REDACTED***")));
}

void testAuthSchemeIsNotMistakenForTheValue(const QString& dir)
{
    const QString path = dir + "/auth_scheme.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.wan"),
        QStringLiteral("Authorization: Basic ZHhwYXNzd29yZHZhbHVl=="));
    report("an auth scheme word is preserved and the value after it is redacted",
           contents.contains(QStringLiteral("Basic"))
           && !contents.contains(QStringLiteral("ZHhwYXNzd29yZHZhbHVl")));
}

void testUrlAuthorityRedaction(const QString& dir)
{
    const QString path = dir + "/urls.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.asr"),
        QStringLiteral("endpoint https://asr.internal.example/v1/stream "
                       "path stays readable"));
    report("URL authority is redacted while the path stays readable",
           !contents.contains(QStringLiteral("asr.internal.example"))
           && contents.contains(QStringLiteral("/v1/stream")));
}

void testDiagnosticFieldsRemainReadable(const QString& dir)
{
    const QString path = dir + "/diagnostics.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.connection"),
        QStringLiteral("callsign=KK7GWY model=FLEX-8600 firmware=3.8.22 "
                       "software_ver=4.2.18.41174 port=4992 slice=0 keytoken=x"));
    report("callsign, model, firmware, version and port stay readable",
           contents.contains(QStringLiteral("KK7GWY"))
           && contents.contains(QStringLiteral("FLEX-8600"))
           && contents.contains(QStringLiteral("3.8.22"))
           && contents.contains(QStringLiteral("4.2.18.41174"))
           && contents.contains(QStringLiteral("port=4992"))
           && contents.contains(QStringLiteral("keytoken=x")));
}

void testRedactionIsIdempotent(const QString& dir)
{
    const QString path = dir + "/idempotent.log";
    const QString line = QStringLiteral(
        "radio at 192.168.50.121 token=ABCDEFGH1234 grid CN87ut "
        "connecting to shack.local mac 00:1C:2D:05:37:2A");
    const QString once = redactPii(line);
    report("redactPii is idempotent", redactPii(once) == once);
    const QString contents = writeAndRead(path, QtDebugMsg,
                                          QStringLiteral("aether.connection"), once);
    report("re-writing an already-redacted line changes nothing further",
           contents.contains(once));
}

// The negative corpus the issue asks for: representative lines from the
// protocols we actually log, each carrying a planted value. Nothing planted
// may survive. Kept as one table so a new log site is one row.
void testNegativeCorpus(const QString& dir)
{
    struct Case { const char* line; const char* planted; };
    static const Case kCases[] = {
        {"Flex status: radio 192.168.50.121 lat=47.6205 lon=-122.3493",  "47.6205"},
        {"SmartLink WAN: id_token=AUDITTOKENVALUE1234 refresh=x",        "AUDITTOKENVALUE1234"},
        {"SmartLink user_settings first_name=AuditGiven last_name=X",    "AuditGiven"},
        {R"(MQTT recv aether/status {"grid_square":"AUDITGRID"})",       "AUDITGRID"},
        {R"(Icom control login for user "auditoperator" accepted)",       "auditoperator"},
        {"MQTT connecting to audit-broker.example.net:8883",             "audit-broker"},
        {"ASR endpoint https://audit-asr.internal/v1",                   "audit-asr"},
        {"FreeDV reported grid AU12di for peer",                         "AU12di"},
        {"exe=/home/auditor/AetherSDR/AetherSDR",                        "/home/auditor"},
        // A valid address: "audi" is not a hextet, and a fixture that is not
        // an address proves nothing about the address rule.
        {"peer 2001:db8::dead:beef established",                          "2001:db8"},
        {"QRZ session for auditor@example.com failed",                   "auditor@example.com"},
        {"rotator disconnected from audit-rotator.lan",                  "audit-rotator"},
    };
    bool allClean = true;
    int index = 0;
    for (const auto& c : kCases) {
        const QString path = QStringLiteral("%1/corpus_%2.log").arg(dir).arg(index++);
        const QString contents = writeAndRead(path, QtDebugMsg,
                                              QStringLiteral("aether.connection"),
                                              QString::fromUtf8(c.line));
        if (contents.contains(QString::fromUtf8(c.planted))) {
            std::printf("       corpus leak: %s\n", c.planted);
            allClean = false;
        }
    }
    report("negative corpus: no planted value survives the writer", allClean);
}


// ---------------------------------------------------------------------------
// Review of #5481 (@jensenpat, @chibondking): each case below reproduces a
// defect that the first round of rules shipped with and the first round of
// tests did not notice.
// ---------------------------------------------------------------------------

// The compressed-IPv6 pattern originally had every group around "::"
// optional, so its shortest match was "::" alone and any C++ qualified name
// in a message was rewritten. 48 log sites stream Class::method as the
// literal start of their message.
void testQualifiedNamesSurviveIpv6Rule(const QString& dir)
{
    const QString path = dir + "/qualified_names.log";
    const QString contents = writeAndRead(
        path, QtWarningMsg, QStringLiteral("aether.connection"),
        QStringLiteral("WanConnection::sendCommand: not connected; "
                       "std::vector allocation failed in "
                       "HidEncoderManager::setKeyImage; Foo::bar"));
    report("C++ qualified names are not mistaken for IPv6 addresses",
           contents.contains(QStringLiteral("WanConnection::sendCommand"))
           && contents.contains(QStringLiteral("std::vector"))
           && contents.contains(QStringLiteral("HidEncoderManager::setKeyImage"))
           && contents.contains(QStringLiteral("Foo::bar"))
           && !contents.contains(QStringLiteral("[v6-redacted]")));
}

// An address written as two-digit hextets is still an address; the MAC rule
// used to consume its first six groups.
void testEightGroupIpv6OfTwoDigitHextets(const QString& dir)
{
    const QString path = dir + "/ipv6_hextets.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.connection"),
        QStringLiteral("peer 20:01:0d:b8:00:00:00:01 established"));
    report("an eight-group IPv6 of two-digit hextets is not read as a MAC",
           contents.contains(QStringLiteral("[v6-redacted]"))
           && !contents.contains(QStringLiteral("**:**")));
}

// A quoted value may contain an escaped quote. A grammar that stops at the
// first '"' ends the match early and leaves the tail of the value in the log
// -- worse than not matching, because the line then looks redacted.
void testEscapedQuoteInsideValue(const QString& dir)
{
    const QString path = dir + "/escaped_quote.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.mqtt"),
        QStringLiteral(R"(payload {"password":"start\"AuditSecretTail"})"));
    report("a value containing an escaped quote is redacted whole",
           !contents.contains(QStringLiteral("AuditSecretTail"))
           && !contents.contains(QStringLiteral("start")));
}

// A Digest header is a comma-separated parameter list, not one value. The
// generic one-value grammar left nonce and response standing.
void testDigestParameterListIsRedacted(const QString& dir)
{
    const QString path = dir + "/digest.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.wan"),
        QStringLiteral("Authorization: Digest username=\"alice\", realm=\"home\", "
                       "nonce=\"AuditNonce\", response=\"AuditResponse\""));
    report("every Digest parameter is redacted, not just the first",
           contents.contains(QStringLiteral("Digest"))
           && !contents.contains(QStringLiteral("AuditNonce"))
           && !contents.contains(QStringLiteral("AuditResponse"))
           && !contents.contains(QStringLiteral("alice")));
}

// The home-root rule ran to the next path separator, so a home directory at
// the end of a field swallowed everything after it on the line.
void testHomeRootDoesNotEatFollowingFields(const QString& dir)
{
    const QString path = dir + "/home_boundary.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.app"),
        QStringLiteral("home=/home/auditor status=connected slice=0"));
    report("a home root does not consume the fields that follow it",
           contents.contains(QStringLiteral("home=~"))
           && contents.contains(QStringLiteral("status=connected"))
           && contents.contains(QStringLiteral("slice=0"))
           && !contents.contains(QStringLiteral("auditor")));
}

// Redaction has to survive a second pass: SupportBundle re-scrubs logs that
// are already clean. A short value used to have its own marker re-eaten,
// giving token=***R***REDACTED***.
void testIdempotentForShortAndMarkedValues(const QString& dir)
{
    const QString line = QStringLiteral(
        "a token=ab b password=x c authorization: Basic ZHhwYXNzd29yZA== "
        "d home=/home/auditor status=up e peer fe80::1%eth0");
    const QString once = redactPii(line);
    const QString twice = redactPii(once);
    report("redaction is idempotent for short and already-marked values",
           once == twice && !once.contains(QStringLiteral("***R***")));
    const QString path = dir + "/idempotent_short.log";
    const QString contents = writeAndRead(path, QtDebugMsg,
                                          QStringLiteral("aether.wan"), once);
    report("re-writing an already-redacted short value changes nothing",
           contents.contains(once));
}

// The host-context keywords are ordinary English. Without a hostname shape
// check they consumed the next word of prose.
void testHostContextKeywordsSpareProse(const QString& dir)
{
    const QString path = dir + "/host_prose.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.audio"),
        QStringLiteral("PipeWireNativeContext: disconnected from PipeWire; "
                       "TLS disconnected from SmartLink server; "
                       "connecting to shack.example.net:1883"));
    report("connection keywords redact hosts but not ordinary prose",
           contents.contains(QStringLiteral("disconnected from PipeWire"))
           && contents.contains(QStringLiteral("disconnected from SmartLink server"))
           && !contents.contains(QStringLiteral("shack.example.net")));
}

// redactPii() runs on EVERY log line, on the writer's hot path, and
// SupportBundle's export loop inherits its cost per line. Building the
// generated regexes per call cost about 1 ms a line; this pins the order of
// magnitude without asserting a precise figure that would flake on slower
// runners.
void testRedactionThroughput()
{
    const QString sample = QStringLiteral(
        "FlexBackend::onStatus: slice 0 freq=14074000 mode=DIGU "
        "callsign=KK7GWY port=4992");
    // Warm the caches so the first-call construction is not timed.
    (void)redactPii(sample);
    QElapsedTimer timer;
    timer.start();
    constexpr int kIterations = 2000;
    for (int i = 0; i < kIterations; ++i)
        (void)redactPii(sample);
    const double usPerLine = double(timer.nsecsElapsed()) / kIterations / 1000.0;
    // Measured ~8 us/line here against ~1000 us/line when the patterns were
    // rebuilt per call. 200 us is far above the former and far below the
    // latter, so it catches a regression of that class without flaking.
    const bool ok = usPerLine < 200.0;
    if (!ok)
        std::printf("       redactPii cost: %.1f us/line\n", usPerLine);
    report("redactPii does not rebuild its patterns per call", ok);
}


// ---------------------------------------------------------------------------
// Second review round on #5481 (@Ozy311, and the still-valid nits from the
// aethersdr-agent pass). Rule INTERACTIONS, which the first two rounds of
// cases missed entirely by testing each rule in isolation.
// ---------------------------------------------------------------------------

// The email rule used to run before the keyword rules, rewriting the head of a
// field's value to the marker; the marker exclusion in the value grammar then
// made the field rule skip the whole field, so the tail survived.
void testEmailInsideCredentialFieldDoesNotMaskIt(const QString& dir)
{
    const QString path = dir + "/email_field_order.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.wan"),
        QStringLiteral("password=person@example.com!AuditTailOne "
                       "token=person@example.com/AuditTailTwo"));
    report("an address inside a field value does not mask the rest of it",
           !contents.contains(QStringLiteral("AuditTailOne"))
           && !contents.contains(QStringLiteral("AuditTailTwo"))
           && !contents.contains(QStringLiteral("person@example.com")));
}

// QDebug doubles the backslash, so a JSON escaped quote inside a value arrives
// as "\\" followed by "\"". The Qt branch terminated inside that pair.
void testQtEncodedEscapedQuoteInsideValue(const QString& dir)
{
    const QString path = dir + "/qt_escaped_quote.log";
    QString formatted;
    QDebug(&formatted) << QByteArray(
        R"({"password":"start\"AuditQtTail","state":"ok"})");
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.mqtt"), formatted);
    report("a Qt-encoded escaped quote does not end the value early",
           !contents.contains(QStringLiteral("AuditQtTail"))
           && !contents.contains(QStringLiteral("start"))
           // the rest of the payload must still be readable
           && contents.contains(QStringLiteral("state")));
}

// The idempotence lookahead sits before the opening quote, so it could not see
// a marker inside one: {"token":"ab"} became {"token":"***R***REDACTED***"}.
void testIdempotentForQuotedShortValues(const QString& dir)
{
    const QString once = redactPii(QStringLiteral(R"({"token":"ab","password":"x"})"));
    const QString twice = redactPii(once);
    report("redaction is idempotent for quoted short values",
           once == twice && !once.contains(QStringLiteral("***R***")));
    const QString path = dir + "/idempotent_quoted.log";
    const QString contents = writeAndRead(path, QtDebugMsg,
                                          QStringLiteral("aether.mqtt"), once);
    report("re-writing an already-redacted quoted value changes nothing",
           contents.contains(once));
}

// "user" is ordinary English before a bare word. The real credential site
// (IcomSession.cpp:273) streams a QString, so QDebug quotes it — the quotes
// are the discriminator.
void testUserRuleTakesQuotedNamesOnly(const QString& dir)
{
    const QString path = dir + "/user_shape.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.icom"),
        QStringLiteral("SmartLinkClient: user settings received; "
                       "ThemeManager: user themes; "
                       "control stream ready - sending login for user \"auditoperator\""));
    report("a quoted user name is redacted and bare prose after \"user\" is not",
           contents.contains(QStringLiteral("user settings received"))
           && contents.contains(QStringLiteral("user themes"))
           && !contents.contains(QStringLiteral("auditoperator")));
}

// A quoted Windows-style home directory whose user name contains a space:
// path="/home/Audit Name" used to keep the surname.
void testQuotedHomeRootWithSpaces(const QString& dir)
{
    const QString path = dir + "/home_quoted.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.app"),
        QStringLiteral(R"(path="/home/Audit Name" status=ok)"));
    report("a quoted home root with a space in the name is redacted whole",
           !contents.contains(QStringLiteral("Audit"))
           && !contents.contains(QStringLiteral("Name"))
           && contents.contains(QStringLiteral("status=ok")));
}

// The connection-keyword rules consume the surrounding quotes; they have to
// put them back or a quoted host loses its quoting.
void testQuotedHostKeepsItsQuotes(const QString& dir)
{
    const QString path = dir + "/host_quotes.log";
    const QString contents = writeAndRead(
        path, QtDebugMsg, QStringLiteral("aether.connection"),
        QStringLiteral("connecting to \"shack.example.net\" now"));
    report("a quoted host keeps its quotes around the marker",
           contents.contains(QStringLiteral("\"***REDACTED***\""))
           && !contents.contains(QStringLiteral("shack.example.net")));
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
    testIpv4QuotedVersionFieldExemption(dir);
    testIpv4ThreeOctetNotRedacted(dir);
    testIpv4QuotedFourOctetIsStillRedacted(dir);
    testSerialRedaction(dir);
    testTokenRedaction(dir);
    testTokenFalsePositiveBoundary(dir);
    testPersonalNameRedaction(dir);
    testCoordinateRedaction(dir);
    testMacDashRedaction(dir);
    testMacColonRedaction(dir);
    testClearLogTruncatesPriorButPreservesSubsequent(dir);
    testShutdownDrainsAllEnqueuedLines(dir);
    testDropAccountingEmitsSummaryAndCounters(dir);
    testHighPriorityReservePreservesCritical(dir);
    testRotationReopenFailureMirrorsToStderr(dir);
    testStderrMirroring(dir);

    testHomePathRedaction(dir);
    testIpv6Redaction(dir);
    testIpv6DoesNotEatMacOrClock(dir);
    testEmailRedaction(dir);
    testUserFieldRedaction(dir);
    testHostContextRedaction(dir);
    testGridRedaction(dir);
    testQuotedAndQtEscapedValueForms(dir);
    testShortValueIsFullyRedacted(dir);
    testAuthSchemeIsNotMistakenForTheValue(dir);
    testUrlAuthorityRedaction(dir);
    testDiagnosticFieldsRemainReadable(dir);
    testRedactionIsIdempotent(dir);
    testNegativeCorpus(dir);

    testQualifiedNamesSurviveIpv6Rule(dir);
    testEightGroupIpv6OfTwoDigitHextets(dir);
    testEscapedQuoteInsideValue(dir);
    testDigestParameterListIsRedacted(dir);
    testHomeRootDoesNotEatFollowingFields(dir);
    testIdempotentForShortAndMarkedValues(dir);
    testHostContextKeywordsSpareProse(dir);
    testRedactionThroughput();

    testEmailInsideCredentialFieldDoesNotMaskIt(dir);
    testQtEncodedEscapedQuoteInsideValue(dir);
    testIdempotentForQuotedShortValues(dir);
    testUserRuleTakesQuotedNamesOnly(dir);
    testQuotedHomeRootWithSpaces(dir);
    testQuotedHostKeepsItsQuotes(dir);

    return g_failed == 0 ? 0 : 1;
}
