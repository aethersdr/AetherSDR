// When an antenna menu has nothing real to offer (AntennaChoiceGate.h).
//
// On a Hermes-Lite 2 the RX/TX antenna buttons and the overlay's RX ANT combo
// offered invented ANT1/ANT2 -- the HL2 has one port and publishes none -- and
// a pick moved the label while nothing moved. The three widgets now ask these
// predicates first and, when refused, emit antennaChoiceRefused() for
// MainWindow to announce through the one-shot unsupported-control notice.
//
// Pinned here:
//   * refused only when CONNECTED and the radio published NO port;
//   * a KiwiSDR virtual receiver keeps the RX menu open (it works on every
//     radio), but never the TX one;
//   * a published list, from the slice or the pan, always opens the menu, so
//     a Flex and an IC-7300MK2 are unchanged.
//
// Not yet mutation-checked. The mutation to run before a PR: dropping
// `connected &&` fails the disconnected rows; dropping `!hasVirtualAntennas`
// fails the Kiwi row.

#include "gui/AntennaChoiceGate.h"

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

static_assert(rxAntennaChoiceRefused(true, false, false), "HL2, no Kiwi: refused");
static_assert(!txAntennaChoiceRefused(true, true), "published TX ports: allowed");

int main()
{
    // RX
    check(rxAntennaChoiceRefused(true, false, false),
          "RX: connected, nothing published, no Kiwi receiver -> refused (HL2)");
    check(!rxAntennaChoiceRefused(true, true, false),
          "RX: connected with published ports -> menu opens (Flex, IC-7300MK2)");
    check(!rxAntennaChoiceRefused(true, false, true),
          "RX: nothing published but a Kiwi receiver on offer -> menu opens");
    check(!rxAntennaChoiceRefused(true, true, true),
          "RX: published ports and Kiwi -> menu opens");
    check(!rxAntennaChoiceRefused(false, false, false),
          "RX: disconnected -> never refused");

    // TX -- no Kiwi escape exists by construction: there is no parameter for it.
    check(txAntennaChoiceRefused(true, false),
          "TX: connected, nothing published -> refused (HL2)");
    check(!txAntennaChoiceRefused(true, true),
          "TX: connected with published ports -> menu opens");
    check(!txAntennaChoiceRefused(false, false),
          "TX: disconnected -> never refused");

    std::printf("antenna_choice_gate_test: %s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
