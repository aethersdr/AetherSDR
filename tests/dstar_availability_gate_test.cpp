#include "gui/DStarAvailabilityGate.h"

#include <cstdio>

using namespace AetherSDR;

int main()
{
    int failures = 0;
    const auto check = [&failures](const char* scenario, bool actual, bool expected) {
        if (actual != expected) {
            std::fprintf(stderr, "FAIL: %s\n", scenario);
            ++failures;
        }
    };

    // Explicit expected outcomes, independent of the production expressions.
    struct TabCase {
        bool connected;
        bool waveforms;
        bool helper;
        bool visible;
    };
    constexpr TabCase tabCases[] = {
        {false, false, false, false}, {false, false, true, true},
        {false, true, false, false},  {false, true, true, true},
        {true, false, false, false},  {true, false, true, false},
        {true, true, false, false},   {true, true, true, true},
    };
    for (const TabCase& row : tabCases) {
        check("tab capability/build matrix",
              dstarTabAvailable(row.connected, row.waveforms, row.helper), row.visible);
    }

    struct StartCase {
        bool connected;
        bool wan;
        bool waveforms;
        bool allowed;
    };
    constexpr StartCase startCases[] = {
        {false, false, false, false}, {false, false, true, false},
        {false, true, false, false},  {false, true, true, false},
        {true, false, false, false},  {true, false, true, true},
        {true, true, false, false},   {true, true, true, false},
    };
    for (const StartCase& row : startCases) {
        check("live service admission matrix",
              dstarServiceCanStart(row.connected, row.wan, row.waveforms), row.allowed);
    }

    // A timer armed on a local waveform-capable session must consult current
    // state when delivered, rather than carrying the original permission.
    check("local waveform session admits helper", dstarServiceCanStart(true, false, true), true);
    check("backend switch revokes pending start", dstarServiceCanStart(true, false, false), false);
    check("disconnect revokes pending start", dstarServiceCanStart(false, false, true), false);
    check("WAN session revokes pending start", dstarServiceCanStart(true, true, true), false);

    if (failures == 0) {
        std::puts("PASS: D-STAR tab and live service admission matrices");
    }
    return failures == 0 ? 0 : 1;
}
