// The BNR pack's failure lines on aether.nvafx (#6285). Socket-free: the
// offline import of a missing archive fails before any I/O, and the URL
// stripper is a pure function. install() is never called; it would start a
// real download on a machine with a supported GPU.

#include "core/NvidiaAfxPack.h"

#include <QCoreApplication>
#include <QDir>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

QStringList g_lines;

void capture(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    const char* level = type == QtWarningMsg ? "W" : type == QtInfoMsg ? "I" : "D";
    g_lines << QStringLiteral("%1 %2: %3")
                   .arg(QLatin1String(level), QLatin1String(ctx.category ? ctx.category : ""), msg);
}

} // namespace

int main(int argc, char** argv)
{
    QTemporaryDir home(QDir::tempPath() + "/aether-afx-pack-log-test-XXXXXX");
    if (!home.isValid()) {
        std::printf("[FAIL] create temporary home\n");
        return 1;
    }
    qputenv("HOME", home.path().toUtf8());
    qputenv("XDG_DATA_HOME", home.path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication app(argc, argv);
    // The product's default rule set: Warning and Info pass, Debug does not.
    QLoggingCategory::setFilterRules(QStringLiteral("aether.*.debug=false"));
    qInstallMessageHandler(capture);

    {
        NvidiaAfxPack pack;
        bool finished = false;
        bool ok = true;
        QString message;
        QObject::connect(&pack, &NvidiaAfxPack::finished, [&](bool o, const QString& m) {
            finished = true;
            ok = o;
            message = m;
        });
        pack.installFromFile(home.filePath("missing-afx-pack.tar.zst"));
        report("missing archive: finished(false)", finished && !ok);
        report("missing archive: message unchanged", message == "archive not found");
        report("missing archive: logged on aether.nvafx as a warning",
               g_lines.contains("W aether.nvafx: NvidiaAfxPack: install failed: archive not found"));
        report("missing archive: pack is not left busy", !pack.busy());
    }

    report("signed query removed",
           NvidiaAfxPack::withoutUrlQueries(
               "Error transferring https://objects.example.com/a/b.tar.zst?X-Amz-Signature=abc&X-Amz-Credential=k - server replied: Forbidden")
               == "Error transferring https://objects.example.com/a/b.tar.zst - server replied: Forbidden");
    report("fragment removed",
           NvidiaAfxPack::withoutUrlQueries("see http://h/x#sha256=deadbeef now") == "see http://h/x now");
    report("text without URLs unchanged",
           NvidiaAfxPack::withoutUrlQueries("checksum mismatch for AFX (corrupt or stale mirror)")
               == "checksum mismatch for AFX (corrupt or stale mirror)");

    qInstallMessageHandler(nullptr);
    std::printf("%s\n", g_failed ? "FAILED" : "PASSED");
    return g_failed ? 1 : 0;
}
