// Unit test for CwSidetoneStartPolicy (#4978).
//
// The CW sidetone is the path a CW operator hears on every element, so the
// decision of which device the backend is handed at start() is pinned here in
// full. The defect this policy fixes was a three-way boolean hidden inline in a
// ~100-line member function: a default selection was always name-matched
// against PortAudio's device list, which on Linux cannot succeed for
// analog/USB descriptions, so every box with no saved output device silently
// ran its sidetone on the QAudioSink push path.
//
// Two rows are load-bearing and are asserted at compile time:
//
//   * no saved device + PortAudio backend  -> BackendDefault   (the fix)
//   * saved device that IS the system default + PortAudio -> Resolved
//     (the documented LIMIT of the fix — the reporting box in #4978 has a
//      saved device and is not reached; #4978 stays open for the
//      escape-hatch follow-up. A refactor that "helpfully" widens this row
//      would change the PR's stated reach.)
//
// Pure policy, so no Qt, no audio backend, no device enumeration — and
// constexpr, so the whole table is also checked at compile time.

#include "core/CwSidetoneStartPolicy.h"

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static int g_checks = 0;

#define EXPECT_START_DEVICE(explicitSel, portAudio, expected, why) do { \
    ++g_checks; \
    const SidetoneStartDevice got_ = sidetoneStartDevice((explicitSel), (portAudio)); \
    if (got_ != (expected)) { \
        std::fprintf(stderr, "FAIL %s:%d  sidetoneStartDevice(explicit=%d, " \
                             "portAudio=%d) = %d, expected %d — %s\n", \
                     __FILE__, __LINE__, int(explicitSel), int(portAudio), \
                     int(got_), int(expected), (why)); \
        ++g_failures; \
    } else { \
        std::printf("[ OK ] %s\n", (why)); \
    } \
} while (0)

#define EXPECT_EXPLICIT(saved, enumerable, expected, why) do { \
    ++g_checks; \
    const bool got_ = isExplicitSidetoneSelection((saved), (enumerable)); \
    if (got_ != (expected)) { \
        std::fprintf(stderr, "FAIL %s:%d  isExplicitSidetoneSelection(saved=%d, " \
                             "enumerable=%d) = %d, expected %d — %s\n", \
                     __FILE__, __LINE__, int(saved), int(enumerable), \
                     int(got_), int(expected), (why)); \
        ++g_failures; \
    } else { \
        std::printf("[ OK ] %s\n", (why)); \
    } \
} while (0)

// ── Compile-time proof of both truth tables ─────────────────────────────────
// A regression that makes any of these false is a build error, not a weekly
// sweep failure.

//                                    explicit  portAudio
static_assert(sidetoneStartDevice(false, true)  == SidetoneStartDevice::BackendDefault,
              "default selection on PortAudio must resolve the backend's own default (#4978)");
static_assert(sidetoneStartDevice(true,  true)  == SidetoneStartDevice::Resolved,
              "explicit selection on PortAudio keeps the resolved device (name match, then QAudioSink fallback)");
static_assert(sidetoneStartDevice(false, false) == SidetoneStartDevice::Resolved,
              "QAudioSink must never be handed a null for a deliberate default selection (it logs it as a fallback)");
static_assert(sidetoneStartDevice(true,  false) == SidetoneStartDevice::Resolved,
              "explicit selection on QAudioSink keeps the resolved device");

//                                          saved  enumerable
static_assert(isExplicitSidetoneSelection(false, false) == false, "nothing saved is not explicit");
static_assert(isExplicitSidetoneSelection(false, true)  == false, "nothing saved is not explicit even if something enumerates");
static_assert(isExplicitSidetoneSelection(true,  false) == false, "a saved device that is gone is not explicit");
static_assert(isExplicitSidetoneSelection(true,  true)  == true,  "a saved, present device is explicit — even when it is the system default");

int main()
{
    // ── sidetoneStartDevice: the four combinations, named by scenario ───────
    EXPECT_START_DEVICE(false, true, SidetoneStartDevice::BackendDefault,
        "Linux/macOS, nothing saved, PortAudio -> backend resolves its own default (the #4978 fix)");
    EXPECT_START_DEVICE(true, true, SidetoneStartDevice::Resolved,
        "Linux/macOS, saved+present device, PortAudio -> resolved device, name-matched (explicit path unchanged)");
    EXPECT_START_DEVICE(false, false, SidetoneStartDevice::Resolved,
        "Windows (no PortAudio) or CwSidetoneBackend=QAudioSink, nothing saved -> resolved device, not null");
    EXPECT_START_DEVICE(true, false, SidetoneStartDevice::Resolved,
        "Windows (no PortAudio) or CwSidetoneBackend=QAudioSink, saved device -> resolved device");

    // ── isExplicitSidetoneSelection: what counts as explicit ───────────────
    EXPECT_EXPLICIT(false, false, false,
        "no AudioOutputDeviceId saved -> not explicit");
    EXPECT_EXPLICIT(true, true, true,
        "saved id still enumerable -> explicit");
    EXPECT_EXPLICIT(true, false, false,
        "saved id no longer enumerable (hotplug window / Q_INVOKABLE entry) -> not explicit");

    // ── The documented limit, end to end ────────────────────────────────────
    // The #4978 reporting box has AudioOutputDeviceId saved and present. It is
    // explicit, so on PortAudio it takes the name-match path exactly as before
    // this fix. This row exists so that reach is a stated fact, not a surprise.
    {
        ++g_checks;
        const bool explicitSel = isExplicitSidetoneSelection(/*saved*/ true, /*enumerable*/ true);
        const SidetoneStartDevice got = sidetoneStartDevice(explicitSel, /*portAudio*/ true);
        if (got != SidetoneStartDevice::Resolved) {
            std::fprintf(stderr, "FAIL %s:%d  a saved device that is the system default must still "
                                 "take the explicit (name-match) path — the #4978 fix does not reach it\n",
                         __FILE__, __LINE__);
            ++g_failures;
        } else {
            std::printf("[ OK ] saved device that IS the system default -> explicit -> Resolved (documented limit; #4978 stays open)\n");
        }
    }

    // ── The fix, end to end ─────────────────────────────────────────────────
    {
        ++g_checks;
        const bool explicitSel = isExplicitSidetoneSelection(/*saved*/ false, /*enumerable*/ false);
        const SidetoneStartDevice got = sidetoneStartDevice(explicitSel, /*portAudio*/ true);
        if (got != SidetoneStartDevice::BackendDefault) {
            std::fprintf(stderr, "FAIL %s:%d  nothing saved on PortAudio must hand the backend a null device\n",
                         __FILE__, __LINE__);
            ++g_failures;
        } else {
            std::printf("[ OK ] nothing saved -> not explicit -> BackendDefault (PortAudio resolves Pa_GetDefaultOutputDevice)\n");
        }
    }

    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
