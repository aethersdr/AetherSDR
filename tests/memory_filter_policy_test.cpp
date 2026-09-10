#include "gui/MemoryFilterPolicy.h"

#include <cstdio>

using namespace AetherSDR;

int main()
{
    int failures = 0;
    const auto check = [&failures](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ++failures;
        }
    };
    RadioCapabilities flex;
    flex.hasProfiles = true;
    const MemoryFilterSpec flexFilters = memoryFilterSpec(
        flex, {QStringLiteral("Local group")}, {QStringLiteral("Global")},
        {QStringLiteral("Transmit")});
    check(flexFilters.label == QStringLiteral("Profile:")
              && flexFilters.names == QStringList({QStringLiteral("Global"),
                                                   QStringLiteral("Transmit")}),
          "connected Flex keeps its global and TX profile vocabulary");
    const MemoryFilterSpec offlineFilters = memoryFilterSpec(
        flex, {QStringLiteral("Local group")}, {QStringLiteral("Stale Flex global")},
        {QStringLiteral("Stale Flex TX")}, true);
    check(offlineFilters.label == QStringLiteral("Group:")
              && offlineFilters.names == QStringList{QStringLiteral("Local group")},
          "offline local bank uses stored groups despite default Flex capabilities");

    RadioCapabilities icom;
    icom.hasProfiles = false;
    icom.canRefreshMemories = true;
    icom.memoryRefreshRequiresGroup = true;
    icom.memoryGroups = {QStringLiteral("Group 00"), QStringLiteral("Group 01")};
    const MemoryFilterSpec icomFilters = memoryFilterSpec(
        icom, {QStringLiteral("Local group")}, {QStringLiteral("Stale Flex global")},
        {QStringLiteral("Stale Flex TX")});
    check(icomFilters.label == QStringLiteral("Group:")
              && icomFilters.names.contains(QStringLiteral("Group 00"))
              && icomFilters.names.contains(QStringLiteral("Local group"))
              && !icomFilters.names.contains(QStringLiteral("Stale Flex global"))
              && !icomFilters.names.contains(QStringLiteral("Stale Flex TX")),
          "Icom offers native and stored groups without stale Flex profiles");
    check(!memoryRefreshSelectionValid(icom, QStringLiteral("Stale Flex global"))
              && memoryRefreshSelectionValid(icom, QStringLiteral("group 01")),
          "group-selecting Icom accepts only a declared native group");
    return failures == 0 ? 0 : 1;
}
