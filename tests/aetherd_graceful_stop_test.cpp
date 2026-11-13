// aetherd's stop signals (POSIX): the first SIGTERM/SIGINT ends the process
// through a normal quit, so teardown releases the radio; a second one, or a loop
// that never gets to quit within the grace period, dies by the signal as before.
// The real daemon is checked end to end, the escalation paths in child processes
// of this test (they cannot be timed against a daemon that quits in milliseconds).
#include "TestSettingsProfile.h"
#include "aetherd/GracefulStop.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <chrono>
#include <csignal>
#include <cstdio>

#include <signal.h>
#include <unistd.h>

using namespace AetherSDR;

namespace {

bool check(bool ok, const char* what)
{
    std::fprintf(stderr, "[%s] %s\n", ok ? "PASS" : "FAIL", what);
    return ok;
}

// ---- child modes (this binary, re-run) ----------------------------------------------

// The quit path: a stop signal arriving while the loop runs ends exec() normally.
int childGraceful(QCoreApplication& app)
{
    const aetherd::GracefulStop stop(app, std::chrono::seconds(5));
    QTimer::singleShot(50, [] { ::raise(SIGTERM); });
    const int rc = app.exec();
    return rc == 0 ? 7 : 1;   // 7: proves exec() returned rather than the default action
}

// Asked twice before the loop could quit: dies by SIGTERM at once.
int childTwice(QCoreApplication& app)
{
    const aetherd::GracefulStop stop(app, std::chrono::seconds(30));
    ::raise(SIGTERM);
    ::raise(SIGTERM);
    ::sleep(10);
    return 1;
}

// A loop that never runs: the grace period ends it by SIGTERM.
int childStuck(QCoreApplication& app)
{
    const aetherd::GracefulStop stop(app, std::chrono::seconds(1));
    ::raise(SIGTERM);
    ::sleep(10);   // the event loop never gets to the self-pipe
    return 1;
}

// An inherited SIG_IGN (nohup) stays ignored.
int childIgnoredHup(QCoreApplication& app)
{
    ::signal(SIGHUP, SIG_IGN);
    const aetherd::GracefulStop stop(app, std::chrono::seconds(1));
    ::raise(SIGHUP);
    QTimer::singleShot(300, [] { QCoreApplication::exit(9); });
    return app.exec();   // 9: still running, SIGHUP did nothing
}

struct Outcome {
    bool finished = false;
    QProcess::ExitStatus status = QProcess::NormalExit;
    int code = -1;
    qint64 ms = 0;
};

Outcome runChild(const QString& self, const QString& mode)
{
    QProcess p;
    QElapsedTimer t;
    t.start();
    p.start(self, {QStringLiteral("--child"), mode});
    Outcome o;
    o.finished = p.waitForFinished(15000);
    if (!o.finished) {
        p.kill();
        p.waitForFinished(1000);
    }
    o.status = p.exitStatus();
    o.code = p.exitCode();
    o.ms = t.elapsed();
    return o;
}

// ---- the real daemon ------------------------------------------------------------------

bool daemonStops(const QString& daemon, const QString& home, int sig, const char* what)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("AETHER_SETTINGS_DIR"), home + QStringLiteral("/settings"));
    env.insert(QStringLiteral("HOME"), home);
    env.insert(QStringLiteral("XDG_CONFIG_HOME"), home + QStringLiteral("/config"));
    env.insert(QStringLiteral("XDG_DATA_HOME"), home + QStringLiteral("/data"));
    env.insert(QStringLiteral("XDG_CACHE_HOME"), home + QStringLiteral("/cache"));
    QProcess p;
    p.setProcessEnvironment(env);
    const QString socket = QStringLiteral("graceful-stop-test-%1").arg(QCoreApplication::applicationPid());
    p.start(daemon, {QStringLiteral("--socket"), socket, QStringLiteral("--discover-sim"),
                     QStringLiteral("--allow-local-control")});
    if (!p.waitForStarted(10000)) {
        return check(false, "daemon started");
    }
    // Serving once it says nothing and is still running; give startup a moment.
    QThread::msleep(1500);
    if (p.state() != QProcess::Running) {
        std::fprintf(stderr, "%s\n", p.readAllStandardError().constData());
        return check(false, "daemon still running before the signal");
    }
    ::kill(static_cast<pid_t>(p.processId()), sig);
    const bool finished = p.waitForFinished(10000);
    if (!finished) {
        p.kill();
        p.waitForFinished(1000);
    }
    const QByteArray err = p.readAllStandardError();
    const bool ok = finished && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0
        && err.contains("received: stopping and releasing the radio");
    if (!ok)
        std::fprintf(stderr, "exit status %d code %d\n%s\n", int(p.exitStatus()), p.exitCode(), err.constData());
    return check(ok, what);
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    if (argc == 3 && QByteArray(argv[1]) == "--child") {
        const QByteArray mode(argv[2]);
        if (mode == "graceful") return childGraceful(app);
        if (mode == "twice") return childTwice(app);
        if (mode == "stuck") return childStuck(app);
        if (mode == "ignored-hup") return childIgnoredHup(app);
        return 1;
    }
    if (argc != 2) {
        std::fprintf(stderr, "expected the aetherd executable path\n");
        return 1;
    }
    TestSettingsProfile profile(QStringLiteral("aetherd-graceful-stop"));
    if (!profile.isValid()) {
        return 1;
    }
    const QString self = QCoreApplication::applicationFilePath();
    bool ok = true;

    const Outcome graceful = runChild(self, QStringLiteral("graceful"));
    ok &= check(graceful.finished && graceful.status == QProcess::NormalExit && graceful.code == 7,
                "a stop signal returns from exec() (normal exit)");

    const Outcome twice = runChild(self, QStringLiteral("twice"));
    ok &= check(twice.finished && twice.status == QProcess::CrashExit && twice.ms < 5000,
                "a second stop signal dies by the signal at once");

    const Outcome stuck = runChild(self, QStringLiteral("stuck"));
    ok &= check(stuck.finished && stuck.status == QProcess::CrashExit && stuck.ms >= 900 && stuck.ms < 5000,
                "a loop that cannot quit dies by the signal when the grace period ends");

    const Outcome hup = runChild(self, QStringLiteral("ignored-hup"));
    ok &= check(hup.finished && hup.status == QProcess::NormalExit && hup.code == 9,
                "an inherited SIG_IGN stays ignored");

    const QString daemon = QString::fromLocal8Bit(argv[1]);
    ok &= daemonStops(daemon, profile.path() + QStringLiteral("/term"), SIGTERM,
                      "aetherd exits 0 on SIGTERM through its normal teardown");
    ok &= daemonStops(daemon, profile.path() + QStringLiteral("/int"), SIGINT,
                      "aetherd exits 0 on SIGINT through its normal teardown");
    return ok ? 0 : 1;
}
