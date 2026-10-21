#include "gui/SharedTxMeterSelection.h"
#include <cstdio>

using namespace AetherSDR;

int main()
{
    struct Case {
        const char* name;
        int radioWatts;
        bool amplifier, kpa, blocked;
        SharedTxMeterSource source;
        int watts;
        bool highPower;
    };
    const Case cases[] = {
        {"radio only", 100, false, false, false, SharedTxMeterSource::Radio, 100, false},
        {"key block without an operating KPA500", 100, false, false, true, SharedTxMeterSource::Radio, 100, false},
        {"KPA500 key pass", 100, false, true, false, SharedTxMeterSource::Amplifier, 500, false},
        {"KPA500 key block", 100, false, true, true, SharedTxMeterSource::RadioKeyBlocked, 100, false},
        {"independent amplifier keeps priority", 100, true, true, true, SharedTxMeterSource::Amplifier, 100, true},
        {"radio limit preserved while blocked", 50, false, true, true, SharedTxMeterSource::RadioKeyBlocked, 50, false},
        {"low-power radio scale preserved", 10, false, true, true, SharedTxMeterSource::RadioKeyBlocked, 10, false},
    };
    int failures = 0;
    for (const Case& c : cases) {
        const SharedTxMeterSelection selected = selectSharedTxMeters(c.radioWatts, c.amplifier, c.kpa, c.blocked);
        const bool ok = selected.source == c.source && selected.maxWatts == c.watts
            && selected.highPowerAmplifier == c.highPower;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", c.name);
        failures += !ok;
    }
    return failures ? 1 : 0;
}
