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
// GUI's path (Hl2EmergencyStop). SIGALRM stands for the stop signal that
// started the grace period; a stray one before any stop dies as SIGALRM.
extern "C" void escalate(int sig)
{
    hl2::fireEmergencyStopAndReraise(sig == SIGALRM && g_received ? int(g_received) : sig);
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

const char* signalName(int sig)
{
    switch (sig) {
    case SIGTERM: return "SIGTERM";
    case SIGINT: return "SIGINT";
    case SIGHUP: return "SIGHUP";
    default: return "a stop signal";
    }
}

// Once per process, from the owner thread: the loop's handler and main()'s
// before-exec check both end a run here, whichever sees the stop first.
bool g_announced = false;
void announceStop()
{
    if (g_announced)
        return;
    g_announced = true;
    QTextStream(stderr) << "aetherd: " << signalName(int(g_received))
                        << " received: stopping and releasing the radio\n";
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
        announceStop();
        QCoreApplication::quit();
    });
    hl2::installSignalHandlerUnlessIgnored(SIGALRM, escalate);
    for (const int sig : {SIGTERM, SIGINT, SIGHUP}) {
        hl2::installSignalHandlerUnlessIgnored(sig, requestStop);
    }
}

GracefulStop::~GracefulStop()
{
    alarm(0); // teardown finished inside the grace period
}

bool GracefulStop::stopRequested() const noexcept
{
    return g_received != 0;
}

bool GracefulStop::takeStopBeforeLoop()
{
    if (!stopRequested())
        return false;
    announceStop();
    return true;
}

#else

namespace {

HANDLE g_finished = nullptr;
DWORD g_graceMs = 0;
volatile LONG g_requested = 0;

// Windows ends a console process about 5 s after a close, logoff or shutdown
// event; the fallback emergency stop has to fire before that.
constexpr DWORD kSessionEndWaitMs = 4000;

// Runs on a thread Windows creates for the event.
BOOL WINAPI onConsoleControl(DWORD type)
{
    if (InterlockedExchange(&g_requested, 1) != 0) {
        hl2::fireEmergencyStop();
        return FALSE; // asked twice: the default handler ends the process
    }
    if (QCoreApplication* app = QCoreApplication::instance()) {
        QMetaObject::invokeMethod(app, [] { QCoreApplication::quit(); }, Qt::QueuedConnection);
    }
    const bool sessionEnd =
        type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT;
    const DWORD wait = sessionEnd && g_graceMs > kSessionEndWaitMs ? kSessionEndWaitMs : g_graceMs;
    if (WaitForSingleObject(g_finished, wait) != WAIT_OBJECT_0) {
        // A stuck loop: the same escalation as POSIX's grace alarm.
        hl2::fireEmergencyStop();
        return FALSE;
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
    // Unregistered first, so no new event reaches a QCoreApplication that is
    // about to be destroyed; then a handler already waiting is released.
    SetConsoleCtrlHandler(onConsoleControl, FALSE);
    if (g_finished) {
        SetEvent(g_finished);
    }
}

bool GracefulStop::stopRequested() const noexcept
{
    return g_requested != 0;
}

bool GracefulStop::takeStopBeforeLoop()
{
    if (!stopRequested())
        return false;
    QTextStream(stderr) << "aetherd: console control event received: stopping and releasing the radio\n";
    return true;
}

#endif

} // namespace AetherSDR::aetherd
