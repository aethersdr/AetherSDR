#include "ThreadName.h"

// No Qt here by design — see the header. Platform detection therefore uses the
// compiler's own predefined macros rather than Q_OS_*.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdlib>
#if defined(__MINGW32__)
// This mingw-w64 header snapshot doesn't declare SetThreadDescription even
// though kernel32.dll exports it (Windows 10 1607+, MSVC's SDK already has
// it). Widening _WIN32_WINNT doesn't help — the prototype is absent outright.
extern "C" __declspec(dllimport) HRESULT WINAPI
    SetThreadDescription(HANDLE hThread, PCWSTR lpThreadDescription);
#endif
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/prctl.h>
#include <unistd.h>
#endif

#include <cstring>

namespace AetherSDR {

void setCurrentThreadName(const char* name)
{
    if (name == nullptr || *name == '\0') {
        return;
    }

#if defined(__linux__)
    // On the thread-group leader, PR_SET_NAME renames the *process* comm as
    // seen by ps/top/pgrep -x/killall (measured: "AetherSDR-GUI" replaced
    // "AetherSDR"). Skip the kernel rename there: the Threads tab then lists
    // the main thread under the process name, as it always did on Linux, and
    // SystemInfo::setCurrentThreadName() still sets the Qt objectName.
    if (getpid() == gettid()) {
        return;
    }
    // 16 bytes including the terminator is a kernel limit, not a convention:
    // prctl fails outright rather than truncating on our behalf.
    char truncated[16];
    std::strncpy(truncated, name, sizeof(truncated) - 1);
    truncated[sizeof(truncated) - 1] = '\0';
    prctl(PR_SET_NAME, truncated, 0, 0, 0);
#elif defined(__APPLE__)
    pthread_setname_np(name);  // current thread only, by design
#elif defined(_WIN32)
    // GetThreadDescription/SetThreadDescription are Windows 10 1607+; the
    // project's documented Windows target is 11 (README "Windows 11").
    const int wide = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    if (wide > 0) {
        auto* buffer = static_cast<wchar_t*>(std::malloc(sizeof(wchar_t) * wide));
        if (buffer != nullptr) {
            if (MultiByteToWideChar(CP_UTF8, 0, name, -1, buffer, wide) > 0) {
                SetThreadDescription(GetCurrentThread(), buffer);
            }
            std::free(buffer);
        }
    }
#else
    (void)name;
#endif
}

}  // namespace AetherSDR
