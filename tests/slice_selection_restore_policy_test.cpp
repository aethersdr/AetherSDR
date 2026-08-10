#include "gui/SliceSelectionRestorePolicy.h"

#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "[FAIL] %s\n", message);
        ++failures;
    } else {
        std::printf("[PASS] %s\n", message);
    }
}

}  // namespace

int main()
{
    using AetherSDR::SliceSelectionRestorePolicy;

    // Radio ID scoping — client station name isolates Multi-Flex instances
    check(SliceSelectionRestorePolicy::radioIdForScope(false, "N12345", "", "Station1") == "N12345/Station1",
          "LAN scope includes station name");
    check(SliceSelectionRestorePolicy::radioIdForScope(true, "", "N12345", "Station1") == "N12345/Station1",
          "WAN scope uses chassis serial and includes station name");
    check(SliceSelectionRestorePolicy::radioIdForScope(true, "", "") == "",
          "WAN scope returns empty when chassis serial is unpopulated");

    // Scope carry guard
    check(SliceSelectionRestorePolicy::scopeCanCarrySelection("N12345/Station1"),
          "non-empty scope ID can carry selection");
    check(!SliceSelectionRestorePolicy::scopeCanCarrySelection(""),
          "empty scope ID cannot carry selection");

    // Persist guard
    check(SliceSelectionRestorePolicy::shouldPersist(false, false, false, false),
          "operator selection is persisted");
    check(!SliceSelectionRestorePolicy::shouldPersist(true, false, false, false),
          "radio echo / system selection is suppressed");
    check(!SliceSelectionRestorePolicy::shouldPersist(false, true, false, false),
          "selection during band recall is suppressed");
    check(!SliceSelectionRestorePolicy::shouldPersist(false, false, true, false),
          "selection during profile load is suppressed");
    check(!SliceSelectionRestorePolicy::shouldPersist(false, false, false, true),
          "selection from restore path is suppressed");

    // Restore action
    check(SliceSelectionRestorePolicy::restoreAction(true, false, true) == SliceSelectionRestorePolicy::RestoreAction::Apply,
          "allowed, unheld, ready scope -> Apply");
    check(SliceSelectionRestorePolicy::restoreAction(true, true, true) == SliceSelectionRestorePolicy::RestoreAction::Defer,
          "profile load held -> Defer");
    check(SliceSelectionRestorePolicy::restoreAction(true, false, false) == SliceSelectionRestorePolicy::RestoreAction::Defer,
          "scope not ready -> Defer");
    check(SliceSelectionRestorePolicy::restoreAction(false, false, true) == SliceSelectionRestorePolicy::RestoreAction::Skip,
          "disallowed -> Skip");

    if (failures == 0) {
        std::printf("\nAll slice selection restore policy tests passed.\n");
        return 0;
    }
    return 1;
}
