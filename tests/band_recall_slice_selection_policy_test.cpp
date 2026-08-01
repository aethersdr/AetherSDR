// Regression coverage for the active-slice command that could undo a FLEX
// band-stack recall and leave the waterfall mapped to the wrong band.

#include "gui/BandRecallSliceSelectionPolicy.h"

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

    if (failures == 0) {
        std::printf("\nAll band-recall slice-selection policy tests passed.\n");
        return 0;
    }
    std::printf("\n%d band-recall slice-selection policy test(s) failed.\n",
                failures);
    return 1;
}
