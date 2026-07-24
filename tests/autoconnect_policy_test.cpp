// Unit coverage for the saved-radio autoconnect + client-slot contention policy
// (#4401). These are the four cases rfoust asked to pin down on the PR:
//   1. a free client slot,
//   2. multiFLEX disabled with an existing client,
//   3. all client slots occupied,
//   4. an automation launch that must remain idle.
// The logic lives in pure functions (gui/AutoConnectPolicy.h) precisely so it
// can be checked here without a running MainWindow or any Qt dialog.

#include "gui/AutoConnectPolicy.h"

#include <iostream>

using aether::ClientSlotAction;
using aether::resolveClientSlotAction;
using aether::savedRadioAutoConnectAllowed;

namespace {

int g_failed = 0;

void expect(bool ok, const char* label)
{
    std::cout << (ok ? "[ OK ] " : "[FAIL] ") << label << '\n';
    if (!ok)
        ++g_failed;
}

const char* name(ClientSlotAction a)
{
    switch (a) {
    case ClientSlotAction::Connect: return "Connect";
    case ClientSlotAction::Decline: return "Decline";
    case ClientSlotAction::Prompt:  return "Prompt";
    }
    return "?";
}

void expectAction(ClientSlotAction got, ClientSlotAction want, const char* label)
{
    const bool ok = got == want;
    std::cout << (ok ? "[ OK ] " : "[FAIL] ") << label
              << " (got " << name(got) << ", want " << name(want) << ")\n";
    if (!ok)
        ++g_failed;
}

} // namespace

int main()
{
    constexpr bool kInteractive = false;
    constexpr bool kAutomation  = true;
    constexpr int  kMaxSlices   = 4;   // e.g. a FLEX-8x00 multiFLEX slot count

    // 1. A free client slot — connect directly, no prompt, no matter the mode.
    expectAction(resolveClientSlotAction(/*multiClientCapable=*/true, /*clients=*/1,
                                         kMaxSlices, kInteractive),
                 ClientSlotAction::Connect, "free slot, interactive → Connect");
    expectAction(resolveClientSlotAction(true, 1, kMaxSlices, kAutomation),
                 ClientSlotAction::Connect, "free slot, automation → Connect");
    expectAction(resolveClientSlotAction(true, 0, kMaxSlices, kAutomation),
                 ClientSlotAction::Connect, "empty radio, automation → Connect");

    // 2. multiFLEX disabled (single-client radio) with a client already on it.
    expectAction(resolveClientSlotAction(/*multiClientCapable=*/false, 1,
                                         kMaxSlices, kInteractive),
                 ClientSlotAction::Prompt,
                 "multiFLEX off + client, interactive → Prompt");
    expectAction(resolveClientSlotAction(false, 1, kMaxSlices, kAutomation),
                 ClientSlotAction::Decline,
                 "multiFLEX off + client, automation → Decline");
    // A single-client radio with no client yet is still connectable.
    expectAction(resolveClientSlotAction(false, 0, kMaxSlices, kAutomation),
                 ClientSlotAction::Connect,
                 "single-client radio, no clients, automation → Connect");

    // 3. All client slots occupied.
    expectAction(resolveClientSlotAction(true, kMaxSlices, kMaxSlices, kInteractive),
                 ClientSlotAction::Prompt, "slots full, interactive → Prompt");
    expectAction(resolveClientSlotAction(true, kMaxSlices, kMaxSlices, kAutomation),
                 ClientSlotAction::Decline, "slots full, automation → Decline");
    expectAction(resolveClientSlotAction(true, kMaxSlices + 1, kMaxSlices, kAutomation),
                 ClientSlotAction::Decline, "over-full slots, automation → Decline");

    // 4. An automation launch that must remain idle: the process-scoped override
    //    suppresses autoconnect even though AutoConnectToLastRadio defaults true;
    //    interactive launches still autoconnect by default.
    expect(savedRadioAutoConnectAllowed(/*override=*/false, /*setting=*/true) == true,
           "interactive default (no override, setting on) → autoconnect");
    expect(savedRadioAutoConnectAllowed(/*override=*/true, /*setting=*/true) == false,
           "automation idle override → no autoconnect");
    expect(savedRadioAutoConnectAllowed(/*override=*/false, /*setting=*/false) == false,
           "setting cleared → no autoconnect");
    expect(savedRadioAutoConnectAllowed(/*override=*/true, /*setting=*/false) == false,
           "override + setting off → no autoconnect");

    if (g_failed == 0)
        std::cout << "\nautoconnect_policy_test: all checks passed\n";
    else
        std::cout << "\nautoconnect_policy_test: " << g_failed << " check(s) FAILED\n";
    return g_failed == 0 ? 0 : 1;
}
