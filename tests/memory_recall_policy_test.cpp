#include "core/MemoryRecallPolicy.h"

#include <QString>

#include <iostream>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

} // namespace

int main()
{
    bool ok = true;

    MasterSDR::MemoryEntry up;
    up.offsetDir = "up";
    up.repeaterOffset = 0.1;
    up.toneMode = "ctcss_tx";
    up.toneValue = 88.5;
    ok &= expect(MasterSDR::memoryRepeaterTxOffsetFreq(up) == 0.1,
                 "up repeater offset produces positive tx_offset_freq");
    up.freq = 146.52;
    ok &= expect(MasterSDR::buildMemoryRecallRetuneCommand(3, up)
                     == "slice tune 3 146.520000 autopan=0",
                 "memory recall retunes slice without autopan");
    ok &= expect(MasterSDR::buildMemoryRecallSliceFixupCommand(3, up)
                     == "slice set 3 repeater_offset_dir=up fm_repeater_offset_freq=0.100000 "
                        "tx_offset_freq=0.100000 fm_tone_mode=ctcss_tx fm_tone_value=88.5",
                 "up repeater memory builds complete slice fixup command");

    MasterSDR::MemoryEntry down;
    down.offsetDir = "DOWN";
    down.repeaterOffset = -0.6;
    down.toneMode = "OFF";
    down.toneValue = 0.0;
    ok &= expect(MasterSDR::memoryRepeaterTxOffsetFreq(down) == -0.6,
                 "down repeater offset produces negative tx_offset_freq");
    ok &= expect(MasterSDR::buildMemoryRecallSliceFixupCommand(7, down)
                     == "slice set 7 repeater_offset_dir=down fm_repeater_offset_freq=0.600000 "
                        "tx_offset_freq=-0.600000 fm_tone_mode=off fm_tone_value=0.0",
                 "down repeater memory normalizes direction, magnitude, and tone mode");

    MasterSDR::MemoryEntry simplex;
    simplex.offsetDir = "simplex";
    simplex.repeaterOffset = 0.6;
    ok &= expect(MasterSDR::memoryRepeaterTxOffsetFreq(simplex) == 0.0,
                 "simplex memory clears tx_offset_freq");
    ok &= expect(MasterSDR::buildMemoryRecallSliceFixupCommand(1, simplex)
                     == "slice set 1 repeater_offset_dir=simplex fm_repeater_offset_freq=0.600000 "
                        "tx_offset_freq=0.000000",
                 "simplex memory still clears stale repeater offset state");

    MasterSDR::MemoryEntry toneOnly;
    toneOnly.toneValue = 123.0;
    ok &= expect(MasterSDR::buildMemoryRecallSliceFixupCommand(2, toneOnly)
                     == "slice set 2 fm_tone_value=123.0",
                 "tone value can be fixed up even when tone mode is absent");

    MasterSDR::MemoryEntry empty;
    ok &= expect(MasterSDR::buildMemoryRecallRetuneCommand(2, empty).isEmpty(),
                 "memory without frequency does not emit retune command");
    ok &= expect(MasterSDR::buildMemoryRecallSliceFixupCommand(2, empty).isEmpty(),
                 "memory without repeater or tone details does not emit a fixup command");

    return ok ? 0 : 1;
}
