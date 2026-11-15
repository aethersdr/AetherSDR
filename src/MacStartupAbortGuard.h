#pragma once

#include <signal.h>

namespace AetherSDR {

// Converts a macOS abort during QApplication construction into a normal
// startup failure. The guard must be disarmed as soon as construction succeeds
// so aborts from the running application retain their normal disposition.
class MacStartupAbortGuard final
{
public:
    static constexpr int kFailureExitCode = 1;

    MacStartupAbortGuard();
    ~MacStartupAbortGuard();

    MacStartupAbortGuard(const MacStartupAbortGuard&) = delete;
    MacStartupAbortGuard& operator=(const MacStartupAbortGuard&) = delete;

    bool isArmed() const { return m_armed; }
    bool disarm();

private:
    struct sigaction m_previousAction {};
    bool m_armed{false};
};

} // namespace AetherSDR
