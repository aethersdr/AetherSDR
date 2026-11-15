#include "MacStartupAbortGuard.h"

#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

namespace {

volatile sig_atomic_t g_previousHandlerCalled = 0;

void previousAbortHandler(int)
{
    g_previousHandlerCalled = 1;
}

int fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    int pipeFds[2] {};
    if (pipe(pipeFds) != 0) {
        return fail("could not create stderr capture pipe");
    }

    const pid_t child = fork();
    if (child < 0) {
        close(pipeFds[0]);
        close(pipeFds[1]);
        return fail("could not fork guarded child");
    }

    if (child == 0) {
        close(pipeFds[0]);
        if (dup2(pipeFds[1], STDERR_FILENO) < 0) {
            _exit(90);
        }
        close(pipeFds[1]);

        AetherSDR::MacStartupAbortGuard guard;
        if (!guard.isArmed()) {
            _exit(91);
        }
        std::abort();
        _exit(92);
    }

    close(pipeFds[1]);
    std::string errorText;
    char buffer[512];
    ssize_t bytesRead = 0;
    while ((bytesRead = read(pipeFds[0], buffer, sizeof(buffer))) > 0) {
        errorText.append(buffer, static_cast<std::size_t>(bytesRead));
    }
    close(pipeFds[0]);

    int childStatus = 0;
    if (waitpid(child, &childStatus, 0) != child) {
        return fail("could not wait for guarded child");
    }
    if (!WIFEXITED(childStatus)
            || WEXITSTATUS(childStatus) != AetherSDR::MacStartupAbortGuard::kFailureExitCode) {
        return fail("guarded SIGABRT did not become exit code 1");
    }
    if (errorText.find("AetherSDR startup error: macOS aborted GUI initialization")
            == std::string::npos) {
        return fail("guarded SIGABRT did not emit the startup error");
    }

    struct sigaction originalAction {};
    if (sigaction(SIGABRT, nullptr, &originalAction) != 0) {
        return fail("could not read the original SIGABRT action");
    }

    struct sigaction previousAction {};
    previousAction.sa_handler = previousAbortHandler;
    sigemptyset(&previousAction.sa_mask);
    if (sigaction(SIGABRT, &previousAction, nullptr) != 0) {
        return fail("could not install the previous SIGABRT action");
    }

    int result = 0;
    {
        AetherSDR::MacStartupAbortGuard guard;
        if (!guard.isArmed()) {
            result = fail("startup guard did not arm");
        } else if (!guard.disarm()) {
            result = fail("startup guard did not disarm");
        }
    }

    if (result == 0) {
        raise(SIGABRT);
        if (g_previousHandlerCalled != 1) {
            result = fail("disarm did not restore the previous SIGABRT action");
        }
    }

    if (sigaction(SIGABRT, &originalAction, nullptr) != 0 && result == 0) {
        result = fail("could not restore the original SIGABRT action");
    }

    if (result == 0) {
        std::puts("macOS startup abort guard: PASS");
    }
    return result;
}
