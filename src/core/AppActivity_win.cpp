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

    // No idle system sleep. A power request is its own object (visible in
    // `powercfg /requests`), so SleepInhibitor's SetThreadExecutionState
    // reset on disconnect cannot clear it. Never closed: held until exit.
    // Idle sleep only, and only until the first sleep: Windows ends power
    // requests on user-initiated sleep (lid, Start > Sleep) and may end them
    // on Modern Standby laptops on battery. Not re-taken after resume.
    REASON_CONTEXT reason{};
    reason.Version = POWER_REQUEST_CONTEXT_VERSION;
    reason.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    reason.Reason.SimpleReasonString =
        const_cast<LPWSTR>(L"AetherSDR audio, DAX and TCI streaming");
    static const HANDLE request = PowerCreateRequest(&reason);
    bool noSleep = false;
    if (request == INVALID_HANDLE_VALUE) {
        qCWarning(lcAudio) << "AppActivity: PowerCreateRequest failed, error"
                           << GetLastError();
    } else if (!PowerSetRequest(request, PowerRequestSystemRequired)) {
        qCWarning(lcAudio) << "AppActivity: PowerSetRequest failed, error"
                           << GetLastError();
    } else {
        noSleep = true;
    }

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
                        << timerError << "(execution-speed opt-out still applies)";
    }

    qCInfo(lcAudio) << "AppActivity: idle sleep blocked" << noSleep
                    << "HighQoS" << (speedError == 0)
                    << "timer resolution honoured" << (timerError == 0);
}

} // namespace AetherSDR
