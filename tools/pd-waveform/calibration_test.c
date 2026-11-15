#include "pd_calibration.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
static struct pd_calibration_reply command(struct pd_calibration* s, unsigned char op, uint64_t n, int64_t now, int eligible)
{
    unsigned char bytes[9] = {op};
    for (unsigned i = 0; i < 8; ++i) { bytes[8 - i] = (unsigned char)(n >> (8 * i)); }
    return pd_calibration_command(s, bytes, sizeof(bytes), now, eligible);
}
int main(void)
{
    struct pd_calibration s = {0};
    assert(!pd_calibration_begin(&s, 100));
    assert(command(&s, 'A', 1, 100, 0).result == PD_CAL_BLOCKED);
    assert(s.phase == PD_CAL_IDLE);
    assert(command(&s, 'A', 2, 100, 1).applied);
    int64_t deadline = s.deadline_ns;
    assert(command(&s, 'A', 2, 500, 1).result == PD_CAL_OK);
    assert(!command(&s, 'A', 2, 500, 1).applied && s.deadline_ns == deadline);
    assert(command(&s, 'X', 2, 500, 1).result == PD_CAL_REPLAY);
    assert(command(&s, 'A', 3, 500, 1).result == PD_CAL_BUSY);
    assert(pd_calibration_begin(&s, 1000));
    assert(!pd_calibration_begin(&s, 1001));
    assert(s.deadline_ns == INT64_C(8000001000));
    pd_calibration_consume(&s, PD_CAL_SAMPLES - 1); assert(s.phase == PD_CAL_RUNNING && s.samples_left == 1);
    pd_calibration_consume(&s, 1); assert(s.phase == PD_CAL_COMPLETED && s.samples_left == 0);
    assert(command(&s, 'A', 2, 2000, 1).result == PD_CAL_REPLAY);
    assert(command(&s, 'A', 4, 2000, 1).applied);
    assert(!pd_calibration_tick(&s, s.deadline_ns)); assert(s.phase == PD_CAL_EXPIRED);
    assert(!command(&s, 'A', 4, 3000, 1).applied && s.phase == PD_CAL_EXPIRED);
    assert(command(&s, 'A', 5, 5000, 1).applied);
    assert(pd_calibration_begin(&s, 6000));
    assert(!pd_calibration_tick(&s, INT64_C(8000006000))); assert(s.phase == PD_CAL_EXPIRED);
    assert(command(&s, 'A', 6, 9000, 1).applied);
    assert(!pd_calibration_tick(&s, 8999)); assert(s.phase == PD_CAL_EXPIRED);
    assert(command(&s, 'A', 7, 10000, 1).applied);
    assert(command(&s, 'X', 8, 10001, 0).applied && s.phase == PD_CAL_CANCELED);
    assert(!pd_calibration_begin(&s, 10002));
    assert(command(&s, '?', 9, 10002, 1).result == PD_CAL_INVALID && s.nonce == 8);
    assert(command(&s, 'A', 0, 10002, 1).result == PD_CAL_INVALID);
    unsigned char bad[10] = {'A'}; assert(pd_calibration_command(&s, bad, sizeof(bad), 10, 1).result == PD_CAL_INVALID);
    assert(command(&s, 'A', 9, INT64_MAX, 1).result == PD_CAL_BLOCKED);
    assert(command(&s, 'A', 10, 0, 1).result == PD_CAL_BLOCKED);
    assert(!strcmp(pd_calibration_name(&s), "canceled"));
    puts("bounded calibration timing/replay tests passed");
    return 0;
}
