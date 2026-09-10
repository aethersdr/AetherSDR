#include "gui/BandRecallSliceSelectionPolicy.h"
#include "gui/ConnectSliceEnumerationGuard.h"

#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* name)
{
    if (condition) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failures;
    }
}

void checkRecallDecision(RadioSliceSelectionSource source, const char* prefix)
{
    const RadioSliceSelectionDecision decision =
        radioSliceSelectionDecision(true, source);
    check(!decision.revealOffscreen, prefix);
    check(decision.suppressActiveCommand,
          source == RadioSliceSelectionSource::ActiveStatus
              ? "active-before-removal: active echo is suppressed"
              : "removal-before-active: fallback active command is suppressed");
}

}  // namespace

int main()
{
    // The captured failure: FLEX first activated the surviving old-band slice.
    checkRecallDecision(
        RadioSliceSelectionSource::ActiveStatus,
        "active-before-removal: old-band slice is not revealed");

    // The inverse valid ordering must be safe too: removing the active slice
    // first makes MainWindow choose a surviving slice as a topology fallback.
    checkRecallDecision(
        RadioSliceSelectionSource::TopologyFallback,
        "removal-before-active: old-band fallback is not revealed");

    // A recreated first slice can arrive before the target pan status. It is
    // also topology synchronization and must not write speculative state.
    const RadioSliceSelectionDecision recreatedFirst =
        radioSliceSelectionDecision(
            true, RadioSliceSelectionSource::TopologyFallback);
    check(!recreatedFirst.revealOffscreen
              && recreatedFirst.suppressActiveCommand,
          "slice-before-pan-status: recreated first slice is synchronization-only");

    // Outside a recall, an external/radio selection retains the established
    // off-screen reveal but never echoes active=1 back to the radio.
    const RadioSliceSelectionDecision externalSelect =
        radioSliceSelectionDecision(
            false, RadioSliceSelectionSource::ActiveStatus);
    check(externalSelect.revealOffscreen
              && externalSelect.suppressActiveCommand,
          "external active status: reveal is preserved without feedback");

    // Ordinary add/remove fallback behavior is unchanged outside a recall.
    const RadioSliceSelectionDecision ordinaryFallback =
        radioSliceSelectionDecision(
            false, RadioSliceSelectionSource::TopologyFallback);
    check(ordinaryFallback.revealOffscreen
              && !ordinaryFallback.suppressActiveCommand,
          "ordinary topology fallback: existing reveal and activation remain");

    // The connect-time bootstrap of the first ENUMERATED slice must never
    // assert active=1. The radio emits status for each slice as it enumerates
    // with its actual live state (active=0 on inactive slices, active=1 on the
    // active slice); asserting active=1 on the first slice during enumeration
    // overwrote the radio's active status before the remaining slices were
    // created client-side, so an operator who left slice B active came back to
    // slice A on every launch.
    const RadioSliceSelectionDecision initialEnumeration =
        radioSliceSelectionDecision(
            false, RadioSliceSelectionSource::InitialEnumeration);
    check(initialEnumeration.revealOffscreen
              && initialEnumeration.suppressActiveCommand,
          "initial enumeration: first slice is selected without asserting active=1");

    // ...and it is equally suppressed during a band recall.
    const RadioSliceSelectionDecision initialDuringRecall =
        radioSliceSelectionDecision(
            true, RadioSliceSelectionSource::InitialEnumeration);
    check(!initialDuringRecall.revealOffscreen
              && initialDuringRecall.suppressActiveCommand,
          "initial enumeration during recall: synchronization-only");

    // TopologyFallback is the ONLY source that may assert the selection — it
    // fires when the active slice was removed and the radio needs to be told
    // which one takes over. Guard that this stays a single exception across
    // all sources.
    const RadioSliceSelectionSource kAllSources[] = {
        RadioSliceSelectionSource::ActiveStatus,
        RadioSliceSelectionSource::TopologyFallback,
        RadioSliceSelectionSource::InitialEnumeration,
    };
    bool onlyFallbackAsserts = true;
    for (const RadioSliceSelectionSource source : kAllSources) {
        const RadioSliceSelectionDecision decision =
            radioSliceSelectionDecision(false, source);
        const bool expectedSuppress =
            (source != RadioSliceSelectionSource::TopologyFallback);
        if (decision.suppressActiveCommand != expectedSuppress) {
            onlyFallbackAsserts = false;
        }
    }
    check(onlyFallbackAsserts,
          "only topology fallback asserts active=1 outside a recall");

    // Wire-level simulation of MainWindow slice adoption sequence on connect.
    // Slice 0 arrives first (inactive), then Slice 1 arrives (active on radio).
    {
        int activeSliceId = -1;
        bool activeCommandSent = false;

        auto processSliceAdded = [&](int sliceId, bool isRadioActive,
                                     RadioSliceSelectionSource firstSliceSource) {
            const bool firstSlice = (activeSliceId < 0);
            if (firstSlice) {
                const auto decision =
                    radioSliceSelectionDecision(false, firstSliceSource);
                activeSliceId = sliceId;
                if (!decision.suppressActiveCommand) {
                    activeCommandSent = true;
                }
            }
            if (isRadioActive && sliceId != activeSliceId) {
                const auto decision = radioSliceSelectionDecision(
                    false, RadioSliceSelectionSource::ActiveStatus);
                activeSliceId = sliceId;
                if (!decision.suppressActiveCommand) {
                    activeCommandSent = true;
                }
            }
        };

        // 1. Correct behavior with InitialEnumeration:
        processSliceAdded(0, false, RadioSliceSelectionSource::InitialEnumeration);
        processSliceAdded(1, true, RadioSliceSelectionSource::InitialEnumeration);

        check(!activeCommandSent,
              "connect enumeration sends no active=1 command on wire");
        check(activeSliceId == 1,
              "connect enumeration converges onto radio-reported active slice");

        // 2. Regression guard: verify that if call site regressed to TopologyFallback,
        // an active=1 command WOULD be emitted on slice 0.
        activeSliceId = -1;
        activeCommandSent = false;
        processSliceAdded(0, false, RadioSliceSelectionSource::TopologyFallback);
        check(activeCommandSent,
              "regression guard: TopologyFallback on connect would improperly send active=1");
    }

    // firstSliceSelectionSource maps initialConnectEnumeration boolean to the right enum
    check(firstSliceSelectionSource(true) == RadioSliceSelectionSource::InitialEnumeration,
          "firstSliceSelectionSource(true) -> InitialEnumeration");
    check(firstSliceSelectionSource(false) == RadioSliceSelectionSource::TopologyFallback,
          "firstSliceSelectionSource(false) -> TopologyFallback");

    // ConnectSliceEnumerationGuard time-window lifecycle & expiration diagnostics
    {
        ConnectSliceEnumerationGuard guard(3000);
        check(!guard.isActive(0), "guard initially inactive");
        check(!guard.isActive(-1), "guard rejects negative nowMs");
        check(!guard.expiredUnused(0), "un-armed guard is not expired-unused");

        guard.arm(1000);
        check(!guard.expiredUnused(1050), "armed guard inside window is not expired-unused");
        check(guard.isActive(1000), "guard active at arm time (consulted)");
        check(guard.isActive(3999), "guard active before window expiry");
        check(!guard.isActive(4000), "guard inactive at window expiry");
        check(!guard.isActive(5000), "guard inactive after window expiry");
        check(!guard.expiredUnused(5000), "guard consulted while active is NOT expired-unused");

        // Expired without being consulted
        ConnectSliceEnumerationGuard unconsultedGuard(3000);
        unconsultedGuard.arm(1000);
        check(!unconsultedGuard.expiredUnused(2000), "inside window: not expired-unused");
        check(unconsultedGuard.expiredUnused(4000), "at expiry without consultation: expired-unused");
        check(unconsultedGuard.expiredUnused(5000), "after expiry without consultation: expired-unused");

        // cancelArm explicitly disarms
        guard.arm(10000);
        check(guard.isActive(10000), "guard armed again");
        guard.cancelArm();
        check(!guard.isActive(10000), "guard inactive after cancelArm");
        check(!guard.expiredUnused(20000), "cancelled guard is not expired-unused");
    }

    if (failures == 0) {
        std::printf("\nAll band-recall slice-selection policy tests passed.\n");
        return 0;
    }
    std::printf("\n%d band-recall slice-selection policy test(s) failed.\n",
                failures);
    return 1;
}
