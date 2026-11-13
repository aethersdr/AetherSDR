#include "GracefulStop.h"

#include "core/backends/hl2/Hl2EmergencyStop.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QTextStream>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <QSocketNotifier>

#include <cerrno>
#include <csignal>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace AetherSDR::aetherd {

#ifndef Q_OS_WIN
namespace {

int g_wake[2] = {-1, -1};                  // self-pipe: [0] read by the loop, [1] written by the handler
volatile std::sig_atomic_t g_received = 0; // the first stop signal, 0 before one
unsigned g_graceSeconds = 0;

// The loop is not going to finish in time, or the operator asked twice: the
// GUI's path. Stop an armed HL2, then die by the signal with the default action
// (same exit status, same core policy). SIGALRM stands for the stop signal that
// started the grace period.
extern "C" void escalate(int sig)
{
    hl2::fireEmergencyStop();
    const int fatal = sig == SIGALRM ? (g_received ? int(g_received) : SIGTERM) : sig;
    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(fatal, &dfl, nullptr);
    raise(fatal);
}

// Async-signal-safe: write(), alarm() and errno only.
extern "C" void requestStop(int sig)
{
    if (g_received) {
        escalate(sig);
        return;
    }
    g_received = sig;
    const int saved = errno;
    const char byte = 1;
    (void)!::write(g_wake[1], &byte, 1);
    alarm(g_graceSeconds);
    errno = saved;
}

void install(int sig, void (*handler)(int))
{
    struct sigaction current {};
    sigaction(sig, nullptr, &current);
    if (current.sa_handler == SIG_IGN) {
        return; // never override an inherited SIG_IGN (nohup ignores SIGHUP)
    }
    struct sigaction action {};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(sig, &action, nullptr);
}

const char* signalName(int sig)
{
    switch (sig) {
    case SIGTERM: return "SIGTERM";
    case SIGINT: return "SIGINT";
    case SIGHUP: return "SIGHUP";
    default: return "a stop signal";
    }
}

} // namespace

GracefulStop::GracefulStop(QCoreApplication& app, std::chrono::seconds grace)
{
    g_graceSeconds = static_cast<unsigned>(grace.count() > 0 ? grace.count() : 1);
    // SIGQUIT (and anything this does not override) keeps the GUI's handler.
    hl2::installEmergencyStopSignalHandlers();
    if (::pipe(g_wake) != 0) {
        QTextStream(stderr) << "aetherd: no stop pipe; a stop signal ends aetherd without releasing the radio\n";
        return;
    }
    for (const int fd : g_wake) {
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    auto* notifier = new QSocketNotifier(g_wake[0], QSocketNotifier::Read, &app);
    QObject::connect(notifier, &QSocketNotifier::activated, &app, [] {
        char drain[16];
        while (::read(g_wake[0], drain, sizeof drain) > 0) {}
        QTextStream(stderr) << "aetherd: " << signalName(int(g_received))
                            << " received: stopping and releasing the radio\n";
        QCoreApplication::quit();
    });
    install(SIGALRM, escalate);
    for (const int sig : {SIGTERM, SIGINT, SIGHUP}) {
        install(sig, requestStop);
    }
}

GracefulStop::~GracefulStop()
{
    alarm(0); // teardown finished inside the grace period
}

#else

namespace {

HANDLE g_finished = nullptr;
DWORD g_graceMs = 0;
volatile LONG g_requested = 0;

BOOL WINAPI onConsoleControl(DWORD type)
{
    if (InterlockedExchange(&g_requested, 1) != 0) {
        hl2::fireEmergencyStop();
        return FALSE; // asked twice: the default handler ends the process
    }
    if (QCoreApplication* app = QCoreApplication::instance()) {
        // This runs on its own thread; a queued call is safe from here.
        QMetaObject::invokeMethod(app, [] { QCoreApplication::quit(); }, Qt::QueuedConnection);
    }
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        // Windows ends the process when this returns: hold it until teardown is done.
        if (WaitForSingleObject(g_finished, g_graceMs) != WAIT_OBJECT_0) {
            hl2::fireEmergencyStop();
        }
    }
    return TRUE;
}

} // namespace

GracefulStop::GracefulStop(QCoreApplication&, std::chrono::seconds grace)
{
    g_graceMs = static_cast<DWORD>(std::chrono::milliseconds(grace).count());
    hl2::installEmergencyStopSignalHandlers();
    g_finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(onConsoleControl, TRUE);
}

GracefulStop::~GracefulStop()
{
    if (g_finished) {
        SetEvent(g_finished);
    }
}

#endif

} // namespace AetherSDR::aetherd
