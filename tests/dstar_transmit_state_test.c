/* D-STAR TX/RX transition lifecycle tests. */

#include "dstar_transmit_state.h"
#include "dstar_tx_output.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    dstar_transmit_state state = DSTAR_TRANSMIT_STATE_INITIALIZER;
    int failed = 0;

    failed += dstar_transmit_state_cancel(&state) != FALSE;
    failed += dstar_transmit_state_consumeReceiveReset(&state) != FALSE;

    failed += dstar_transmit_state_begin(&state) != TRUE;
    failed += dstar_transmit_state_begin(&state) != FALSE;
    failed += dstar_transmit_state_phase(&state) != DSTAR_TRANSMIT_ACTIVE;
    failed += dstar_transmit_state_transmitOrTailEnabled(&state) != TRUE;
    failed += dstar_transmit_state_cancel(&state) != TRUE;
    failed += dstar_transmit_state_consumeReceiveReset(&state) != TRUE;
    failed += dstar_transmit_state_consumeReceiveReset(&state) != FALSE;

    failed += dstar_transmit_state_begin(&state) != TRUE;
    failed += dstar_transmit_state_requestEnd(&state) != TRUE;
    failed += dstar_transmit_state_phase(&state) != DSTAR_TRANSMIT_DRAINING;
    failed += dstar_transmit_state_cancel(&state) != TRUE;
    failed += dstar_transmit_state_phase(&state) != DSTAR_TRANSMIT_IDLE;
    failed += dstar_transmit_state_consumeReceiveReset(&state) != TRUE;

    failed += dstar_transmit_state_begin(&state) != TRUE;
    failed += dstar_transmit_state_requestEnd(&state) != TRUE;
    failed += dstar_transmit_state_requestEnd(&state) != FALSE;
    failed += dstar_transmit_state_phase(&state) != DSTAR_TRANSMIT_DRAINING;
    failed += dstar_transmit_state_transmitOrTailEnabled(&state) != TRUE;
    failed += dstar_transmit_state_consumeReceiveReset(&state) != FALSE;
    failed += dstar_transmit_state_markEndQueued(&state) != TRUE;
    failed += dstar_transmit_state_markEndQueued(&state) != FALSE;
    failed += dstar_transmit_state_phase(&state) != DSTAR_TRANSMIT_ENDING;
    dstar_transmit_state_finishTail(&state);
    failed += dstar_transmit_state_phase(&state) != DSTAR_TRANSMIT_IDLE;
    failed += dstar_transmit_state_transmitOrTailEnabled(&state) != FALSE;
    failed += dstar_transmit_state_consumeReceiveReset(&state) != TRUE;

    dstar_transmit_state_reset(&state);
    failed += dstar_transmit_state_consumeReceiveReset(&state) != FALSE;

    failed += fabsf(dstar_tx_output_scaleSample(0.5f, 0.5f) - 0.25f) > 0.0001f;
    failed += dstar_tx_output_scaleSample(2.0f, 1.0f) != 0.98f;
    failed += dstar_tx_output_scaleSample(-2.0f, 1.0f) != -0.98f;
    const float not_a_number = strtof("NAN", NULL);
    const float positive_infinity = strtof("INF", NULL);
    failed += dstar_tx_output_scaleSample(not_a_number, 0.5f) != 0.0f;
    failed += dstar_tx_output_scaleSample(0.5f, not_a_number) != 0.0f;
    failed += dstar_tx_output_scaleSample(positive_infinity, 0.5f) != 0.0f;
    BOOL clipped = FALSE;
    BOOL invalid = FALSE;
    failed += dstar_tx_output_pcm16(0.5f, &clipped, &invalid) != 16383;
    failed += clipped || invalid;
    failed += dstar_tx_output_pcm16(2.0f, &clipped, &invalid) != 32767;
    failed += !clipped || invalid;
    failed += dstar_tx_output_pcm16(-2.0f, &clipped, &invalid) != -32768;
    failed += !clipped || invalid;
    failed += dstar_tx_output_pcm16(not_a_number, &clipped, &invalid) != 0;
    failed += clipped || !invalid;
    failed += dstar_tx_output_packetIntervalUsec(128U, 24000U) != 5333U;
    failed += dstar_tx_output_packetIntervalUsec(128U, 0U) != 0U;

    printf("%s D-STAR transmit lifecycle and output policy\n",
           failed == 0 ? "[ OK ]" : "[FAIL]");
    return failed == 0 ? 0 : 1;
}
