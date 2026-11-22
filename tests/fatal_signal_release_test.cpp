// The family-neutral termination-signal release (#7163): the handlers both
// main()s install call every registered release, then die by the signal; an
// inherited SIG_IGN stays ignored; an armed HL2 registers its stop datagram.
// Each case runs in a forked child so the static table starts empty. No
// sockets: releases report over a pipe, and the HL2 is armed on a pipe fd and
// never fired (hl2_signal_stop_test sends the real datagram). POSIX only.

#include "core/FatalSignalRelease.h"
#include "core/backends/hl2/Hl2EmergencyStop.h"

#include <QHostAddress>

#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <string>

#include <sys/resource.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

using namespace AetherSDR;

namespace {

int g_failures = 0;
void check(bool ok, const std::string& what)
{
    std::fprintf(stderr, "%s %s\n", ok ? "[ OK ]" : "FAIL:", what.c_str());
    if (!ok)
        ++g_failures;
}

int g_report = -1;   // the child's write end; written from the handler

template <char Tag>
void reportRelease() noexcept
{
    const char byte = Tag;
    (void)!::write(g_report, &byte, 1);
}

struct ChildResult {
    int status = 0;
    std::string reported;   // every byte the child's releases wrote
};

// Runs `body` in a child and returns its wait status and what it reported.
template <typename Body>
ChildResult inChild(Body body)
{
    int fds[2];
    if (::pipe(fds) != 0) {
        std::perror("pipe");
        std::_Exit(2);
    }
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(fds[0]);
        g_report = fds[1];
        const rlimit noCore{0, 0};
        ::setrlimit(RLIMIT_CORE, &noCore);   // SIGQUIT would dump core
        std::_Exit(body());
    }
    ::close(fds[1]);
    ChildResult r;
    char buf[64];
    ssize_t n;
    while ((n = ::read(fds[0], buf, sizeof buf)) > 0)
        r.reported.append(buf, static_cast<size_t>(n));
    ::close(fds[0]);
    ::waitpid(pid, &r.status, 0);
    return r;
}

bool diedBy(const ChildResult& r, int sig)
{
    return WIFSIGNALED(r.status) && WTERMSIG(r.status) == sig;
}

bool exitedWith(const ChildResult& r, int code)
{
    return WIFEXITED(r.status) && WEXITSTATUS(r.status) == code;
}

}  // namespace

int main()
{
    // The table: idempotent, queryable, null refused, bounded.
    const ChildResult table = inChild([] {
        int bad = 0;
        if (!registerFatalSignalRelease(reportRelease<'a'>)) bad |= 1;
        if (!registerFatalSignalRelease(reportRelease<'a'>)) bad |= 2;
        if (!isFatalSignalReleaseRegistered(reportRelease<'a'>)) bad |= 4;
        if (isFatalSignalReleaseRegistered(reportRelease<'b'>)) bad |= 8;
        if (registerFatalSignalRelease(nullptr)) bad |= 16;
        fireFatalSignalRelease();
        const FatalSignalReleaseFn more[] = {
            reportRelease<'1'>, reportRelease<'2'>, reportRelease<'3'>, reportRelease<'4'>,
            reportRelease<'5'>, reportRelease<'6'>, reportRelease<'7'>};
        for (const auto fn : more)
            if (!registerFatalSignalRelease(fn)) bad |= 32;
        if (registerFatalSignalRelease(reportRelease<'8'>)) bad |= 64;   // ninth: full
        return bad;
    });
    check(exitedWith(table, 0),
          "register is idempotent and queryable, refuses null, and refuses a ninth release (child "
              + std::to_string(WEXITSTATUS(table.status)) + ")");
    check(table.reported == "a", "a release registered twice fires once (got '" + table.reported + "')");

    // Every termination signal fires each release once, then dies by that signal.
    for (const int sig : {SIGTERM, SIGINT, SIGHUP, SIGQUIT}) {
        const ChildResult r = inChild([sig] {
            std::signal(sig, SIG_DFL);   // a background job or nohup inherits SIG_IGN
            registerFatalSignalRelease(reportRelease<'x'>);
            registerFatalSignalRelease(reportRelease<'y'>);
            installFatalSignalRelease();
            std::raise(sig);
            return 3;   // survived the signal
        });
        const std::string name = std::to_string(sig);
        check(r.reported == "xy", "signal " + name + " fires both releases (got '" + r.reported + "')");
        check(diedBy(r, sig), "signal " + name + " still kills the process by that signal");
    }

    // Nothing registered: the handler is a pass-through.
    const ChildResult empty = inChild([] {
        std::signal(SIGTERM, SIG_DFL);
        installFatalSignalRelease();
        std::raise(SIGTERM);
        return 3;
    });
    check(diedBy(empty, SIGTERM) && empty.reported.empty(),
          "with no release registered, SIGTERM kills as before");

    // nohup: an inherited SIG_IGN is left ignored, and nothing fires.
    const ChildResult ignored = inChild([] {
        std::signal(SIGHUP, SIG_IGN);
        registerFatalSignalRelease(reportRelease<'h'>);
        installFatalSignalRelease();
        std::raise(SIGHUP);
        return 0;
    });
    check(exitedWith(ignored, 0) && ignored.reported.empty(),
          "an inherited SIG_IGN for SIGHUP stays ignored and fires nothing");

    // The HL2 joins the table when it arms, and not on a refused arm.
    const ChildResult hl2 = inChild([] {
        int bad = 0;
        const std::array<std::uint8_t, 64> packet{};
        const QHostAddress host(QStringLiteral("127.0.0.1"));
        if (isFatalSignalReleaseRegistered(hl2::fireEmergencyStop)) bad |= 1;
        hl2::armEmergencyStop(-1, host, 1024, packet);
        if (isFatalSignalReleaseRegistered(hl2::fireEmergencyStop)) bad |= 2;
        hl2::armEmergencyStop(g_report, host, 1024, packet);   // any fd; never fired
        if (!isFatalSignalReleaseRegistered(hl2::fireEmergencyStop)) bad |= 4;
        hl2::disarmEmergencyStop();
        return bad;
    });
    check(exitedWith(hl2, 0),
          "an armed HL2 registers its emergency stop; a refused arm does not (child "
              + std::to_string(WEXITSTATUS(hl2.status)) + ")");

    std::fprintf(stderr, "%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
