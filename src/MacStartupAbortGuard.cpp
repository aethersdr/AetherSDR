#include "MacStartupAbortGuard.h"

#include <unistd.h>

namespace {

constexpr char kStartupAbortMessage[] =
    "AetherSDR startup error: macOS aborted GUI initialization. "
    "GUI services may be unavailable (for example, in a restricted sandbox). "
    "Launch AetherSDR from Finder or a normal Terminal session.\n";

void handleStartupAbort(int)
{
    // QApplication construction can abort from inside AppKit/HIServices before
    // Qt logging exists. Only async-signal-safe operations are valid here.
    const ssize_t ignored = write(STDERR_FILENO,
                                  kStartupAbortMessage,
                                  sizeof(kStartupAbortMessage) - 1);
    (void)ignored;
    _exit(AetherSDR::MacStartupAbortGuard::kFailureExitCode);
}

} // namespace

namespace AetherSDR {

MacStartupAbortGuard::MacStartupAbortGuard()
{
    struct sigaction action {};
    action.sa_handler = handleStartupAbort;
    sigemptyset(&action.sa_mask);

    m_armed = sigaction(SIGABRT, &action, &m_previousAction) == 0;
}

MacStartupAbortGuard::~MacStartupAbortGuard()
{
    (void)disarm();
}

bool MacStartupAbortGuard::disarm()
{
    if (!m_armed) {
        return true;
    }

    if (sigaction(SIGABRT, &m_previousAction, nullptr) != 0) {
        return false;
    }

    m_armed = false;
    return true;
}

} // namespace AetherSDR
