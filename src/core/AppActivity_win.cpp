#include "AppActivity.h"
#include "LogManager.h"

#include <windows.h>

// Windows 11 SDK name; older SDKs lack it. Windows 10 may reject the bit by
// failing the whole call, so execution speed is applied on its own first.
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

namespace AetherSDR {

namespace {
// Returns 0 on success, else the Windows error code.
DWORD disableThrottling(ULONG mask)
{
    // ControlMask set + StateMask clear = "this policy is OFF for us".
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = mask;
    state.StateMask = 0;
    if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                              &state, sizeof(state))) {
        return 0;
    }
    return GetLastError();
}
} // namespace

void keepAppActive()
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;

    // Sleep is not touched here: "Prevent system sleep while connected"
    // (SleepInhibitor) is the only idle-sleep block.

    // HighQoS: never run as EcoQoS (efficiency cores, reduced clocks).
    // Each call REPLACES the control mask, so the second call must carry
    // both bits; if Windows 10 rejects the timer bit, this first call stands.
    const DWORD speedError = disableThrottling(PROCESS_POWER_THROTTLING_EXECUTION_SPEED);
    if (speedError != 0) {
        qCWarning(lcAudio) << "AppActivity: execution-speed throttling opt-out failed, error"
                           << speedError;
    }
    // Also keep Qt's 1 ms precise timers when minimized/occluded and silent
    // (Windows 11). Rejection is expected on older Windows.
    const DWORD timerError = disableThrottling(PROCESS_POWER_THROTTLING_EXECUTION_SPEED
                                               | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION);
    if (timerError != 0) {
        qCInfo(lcAudio) << "AppActivity: timer-resolution opt-out unsupported, error"
                        << timerError << (speedError == 0
                            ? "(execution-speed opt-out still applies)"
                            : "(execution-speed opt-out also failed)");
    }

    // Either call carrying EXECUTION_SPEED leaves HighQoS in effect.
    qCInfo(lcAudio) << "AppActivity: HighQoS" << (speedError == 0 || timerError == 0)
                    << "timer resolution honoured" << (timerError == 0);
}

} // namespace AetherSDR
