/* Digital-voice interlock eligibility and transition tests. */

#include "digital_voice_tx_gate.h"

#include <stdio.h>

static int failures = 0;

static void expect_action(const char * name,
                          digital_voice_tx_gate_action actual,
                          digital_voice_tx_gate_action expected)
{
    const BOOL passed = actual == expected;
    printf("%s %s\n", passed ? "[ OK ]" : "[FAIL]", name);
    if (!passed) {
        failures++;
    }
}

static void expect_true(const char * name, BOOL condition)
{
    printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", name);
    if (!condition) {
        failures++;
    }
}

static digital_voice_tx_gate ready_gate(void)
{
    digital_voice_tx_gate gate = DIGITAL_VOICE_TX_GATE_INITIALIZER;
    digital_voice_tx_gate_setModeSlice(&gate, TRUE, 2U);
    digital_voice_tx_gate_setTxSlice(&gate, TRUE, 2U);
    return gate;
}

int main(void)
{
    digital_voice_tx_gate gate = ready_gate();
    expect_action("operator PTT begins on the active TX slice",
                  digital_voice_tx_gate_observe(
                      &gate, DIGITAL_VOICE_TX_EVENT_PTT_REQUESTED,
                      TRUE, DIGITAL_VOICE_TX_SOURCE_HARDWARE),
                  DIGITAL_VOICE_TX_GATE_BEGIN);
    expect_action("duplicate transmitting status is idempotent",
                  digital_voice_tx_gate_observe(
                      &gate, DIGITAL_VOICE_TX_EVENT_TRANSMITTING,
                      TRUE, DIGITAL_VOICE_TX_SOURCE_HARDWARE),
                  DIGITAL_VOICE_TX_GATE_NONE);
    expect_action("UNKEY without a source requests one tail",
                  digital_voice_tx_gate_observe(
                      &gate, DIGITAL_VOICE_TX_EVENT_UNKEY_REQUESTED,
                      FALSE, DIGITAL_VOICE_TX_SOURCE_UNKNOWN),
                  DIGITAL_VOICE_TX_GATE_REQUEST_END);
    expect_action("duplicate UNKEY is idempotent",
                  digital_voice_tx_gate_observe(
                      &gate, DIGITAL_VOICE_TX_EVENT_UNKEY_REQUESTED,
                      FALSE, DIGITAL_VOICE_TX_SOURCE_UNKNOWN),
                  DIGITAL_VOICE_TX_GATE_NONE);
    expect_action("READY terminates the transaction",
                  digital_voice_tx_gate_observe(
                      &gate, DIGITAL_VOICE_TX_EVENT_READY,
                      FALSE, DIGITAL_VOICE_TX_SOURCE_UNKNOWN),
                  DIGITAL_VOICE_TX_GATE_CANCEL);

    gate = ready_gate();
    expect_action("PTT without a classified source never keys TX",
                  digital_voice_tx_gate_observe(
                      &gate, DIGITAL_VOICE_TX_EVENT_PTT_REQUESTED,
                      FALSE, DIGITAL_VOICE_TX_SOURCE_UNKNOWN),
                  DIGITAL_VOICE_TX_GATE_NONE);
    expect_action("classifying the source afterwards then begins",
                  digital_voice_tx_gate_observe(
                      &gate, DIGITAL_VOICE_TX_EVENT_TRANSMITTING,
                      TRUE, DIGITAL_VOICE_TX_SOURCE_HARDWARE),
                  DIGITAL_VOICE_TX_GATE_BEGIN);

    gate = ready_gate();
    expect_action("TUNE never starts digital voice",
                  digital_voice_tx_gate_observe(
                      &gate, DIGITAL_VOICE_TX_EVENT_PTT_REQUESTED,
                      TRUE, DIGITAL_VOICE_TX_SOURCE_TUNE),
                  DIGITAL_VOICE_TX_GATE_NONE);
    expect_action("source-less TUNE unkey remains inhibited",
                  digital_voice_tx_gate_observe(
                      &gate, DIGITAL_VOICE_TX_EVENT_UNKEY_REQUESTED,
                      FALSE, DIGITAL_VOICE_TX_SOURCE_UNKNOWN),
                  DIGITAL_VOICE_TX_GATE_NONE);

    gate = ready_gate();
    digital_voice_tx_gate_setExpectedOwner(&gate, TRUE, 0x12345678U);
    digital_voice_tx_gate_setTxOwner(&gate, TRUE, 0x87654321U);
    expect_action("foreign software TX owner cannot start digital voice",
                  digital_voice_tx_gate_observe(
                      &gate, DIGITAL_VOICE_TX_EVENT_PTT_REQUESTED,
                      TRUE, DIGITAL_VOICE_TX_SOURCE_SOFTWARE),
                  DIGITAL_VOICE_TX_GATE_NONE);
    digital_voice_tx_gate_observe(
        &gate, DIGITAL_VOICE_TX_EVENT_READY,
        FALSE, DIGITAL_VOICE_TX_SOURCE_UNKNOWN);
    digital_voice_tx_gate_setTxOwner(&gate, TRUE, 0x12345678U);
    expect_action("matching software TX owner starts digital voice",
                  digital_voice_tx_gate_observe(
                      &gate, DIGITAL_VOICE_TX_EVENT_PTT_REQUESTED,
                      TRUE, DIGITAL_VOICE_TX_SOURCE_SOFTWARE),
                  DIGITAL_VOICE_TX_GATE_BEGIN);
    expect_action("foreign ownership cancels active digital voice",
                  digital_voice_tx_gate_setTxOwner(
                      &gate, TRUE, 0x87654321U),
                  DIGITAL_VOICE_TX_GATE_CANCEL);

    gate = ready_gate();
    digital_voice_tx_gate_setExpectedOwner(&gate, TRUE, 0x12345678U);
    expect_action("physical PTT remains operator intent without a client owner",
                  digital_voice_tx_gate_observe(
                      &gate, DIGITAL_VOICE_TX_EVENT_PTT_REQUESTED,
                      TRUE, DIGITAL_VOICE_TX_SOURCE_HARDWARE),
                  DIGITAL_VOICE_TX_GATE_BEGIN);

    gate = (digital_voice_tx_gate)DIGITAL_VOICE_TX_GATE_INITIALIZER;
    digital_voice_tx_gate_setTxSlice(&gate, TRUE, 2U);
    digital_voice_tx_gate_setTxOwner(&gate, TRUE, 0x87654321U);
    digital_voice_tx_gate_observe(
        &gate, DIGITAL_VOICE_TX_EVENT_PTT_REQUESTED,
        TRUE, DIGITAL_VOICE_TX_SOURCE_SOFTWARE);
    digital_voice_tx_gate_setExpectedOwner(&gate, TRUE, 0x12345678U);
    expect_action("foreign PTT already active at startup remains inhibited",
                  digital_voice_tx_gate_setModeSlice(&gate, TRUE, 2U),
                  DIGITAL_VOICE_TX_GATE_NONE);
    gate = ready_gate();
    digital_voice_tx_gate_setTxSlice(&gate, TRUE, 3U);
    expect_action("wrong TX slice inhibits PTT",
                  digital_voice_tx_gate_observe(
                      &gate, DIGITAL_VOICE_TX_EVENT_PTT_REQUESTED,
                      TRUE, DIGITAL_VOICE_TX_SOURCE_HARDWARE),
                  DIGITAL_VOICE_TX_GATE_NONE);

    gate = (digital_voice_tx_gate)DIGITAL_VOICE_TX_GATE_INITIALIZER;
    digital_voice_tx_gate_setAuthoritativeSelection(&gate, TRUE, 5U, FALSE);
    digital_voice_tx_gate_setSliceStatus(
        &gate, 2U, TRUE, TRUE, TRUE, TRUE, TRUE, FALSE);
    expect_action("DSTR receive on another slice does not own TX",
                  digital_voice_tx_gate_observe(
                      &gate, DIGITAL_VOICE_TX_EVENT_PTT_REQUESTED,
                      TRUE, DIGITAL_VOICE_TX_SOURCE_HARDWARE),
                  DIGITAL_VOICE_TX_GATE_NONE);
    expect_true("another-mode TX slice remains ineligible",
                !digital_voice_tx_gate_isEligible(&gate));

    expect_action("moving TX to the DSTR slice begins while PTT is active",
                  digital_voice_tx_gate_setAuthoritativeSelection(
                      &gate, TRUE, 2U, TRUE),
                  DIGITAL_VOICE_TX_GATE_BEGIN);
    expect_action("moving TX away from DSTR cancels waveform TX",
                  digital_voice_tx_gate_setAuthoritativeSelection(
                      &gate, TRUE, 5U, FALSE),
                  DIGITAL_VOICE_TX_GATE_CANCEL);

    gate = (digital_voice_tx_gate)DIGITAL_VOICE_TX_GATE_INITIALIZER;
    digital_voice_tx_gate_setAuthoritativeSelection(&gate, TRUE, 7U, TRUE);
    digital_voice_tx_gate_setSliceStatus(
        &gate, 3U, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE);
    expect_true("non-selected DSTR status cannot replace authoritative TX slice",
                gate.tx_slice_valid && gate.tx_slice == 7U
                    && digital_voice_tx_gate_isEligible(&gate));
    digital_voice_tx_gate_setSliceStatus(
        &gate, 7U, FALSE, TRUE, TRUE, FALSE, FALSE, FALSE);
    expect_true("selected TX slice leaving DSTR becomes ineligible",
                !digital_voice_tx_gate_isEligible(&gate));

    gate = (digital_voice_tx_gate)DIGITAL_VOICE_TX_GATE_INITIALIZER;
    digital_voice_tx_gate_observe(
        &gate, DIGITAL_VOICE_TX_EVENT_PTT_REQUESTED,
        TRUE, DIGITAL_VOICE_TX_SOURCE_HARDWARE);
    digital_voice_tx_gate_observe(
        &gate, DIGITAL_VOICE_TX_EVENT_UNKEY_REQUESTED,
        FALSE, DIGITAL_VOICE_TX_SOURCE_UNKNOWN);
    expect_action("late TX selection after unkey does not start a waveform",
                  digital_voice_tx_gate_setAuthoritativeSelection(
                      &gate, TRUE, 4U, TRUE),
                  DIGITAL_VOICE_TX_GATE_NONE);

    gate = ready_gate();
    digital_voice_tx_gate_observe(
        &gate, DIGITAL_VOICE_TX_EVENT_PTT_REQUESTED,
        TRUE, DIGITAL_VOICE_TX_SOURCE_HARDWARE);
    expect_action("changing TX slice cancels an active transaction",
                  digital_voice_tx_gate_setTxSlice(&gate, TRUE, 3U),
                  DIGITAL_VOICE_TX_GATE_CANCEL);

    gate = (digital_voice_tx_gate)DIGITAL_VOICE_TX_GATE_INITIALIZER;
    expect_action("combined slice status establishes mode and TX ownership",
                  digital_voice_tx_gate_setSliceStatus(
                      &gate, 7U, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE),
                  DIGITAL_VOICE_TX_GATE_NONE);
    expect_action("combined slice status can begin transmit",
                  digital_voice_tx_gate_observe(
                      &gate, DIGITAL_VOICE_TX_EVENT_TRANSMITTING,
                      TRUE, DIGITAL_VOICE_TX_SOURCE_HARDWARE),
                  DIGITAL_VOICE_TX_GATE_BEGIN);
    expect_action("slice removal overrides stale mode and TX fields",
                  digital_voice_tx_gate_setSliceStatus(
                      &gate, 7U, TRUE, FALSE, TRUE, TRUE, TRUE, TRUE),
                  DIGITAL_VOICE_TX_GATE_CANCEL);
    expect_true("removed slice clears mode ownership",
                !gate.mode_slice_valid);
    expect_true("removed slice clears TX ownership",
                !gate.tx_slice_valid);

    const digital_voice_tx_event terminal_events[] = {
        DIGITAL_VOICE_TX_EVENT_RECEIVE,
        DIGITAL_VOICE_TX_EVENT_NOT_READY,
        DIGITAL_VOICE_TX_EVENT_TX_FAULT,
        DIGITAL_VOICE_TX_EVENT_TIMEOUT,
        DIGITAL_VOICE_TX_EVENT_STUCK_INPUT
    };
    for (uint32 i = 0U;
         i < sizeof(terminal_events) / sizeof(terminal_events[0]);
         i++) {
        gate = ready_gate();
        digital_voice_tx_gate_observe(
            &gate, DIGITAL_VOICE_TX_EVENT_PTT_REQUESTED,
            TRUE, DIGITAL_VOICE_TX_SOURCE_HARDWARE);
        expect_action("terminal interlock state cancels TX",
                      digital_voice_tx_gate_observe(
                          &gate, terminal_events[i],
                          FALSE, DIGITAL_VOICE_TX_SOURCE_UNKNOWN),
                      DIGITAL_VOICE_TX_GATE_CANCEL);
    }

    printf("\n%s digital-voice TX gate tests.\n",
           failures == 0 ? "All" : "Failed");
    return failures == 0 ? 0 : 1;
}
