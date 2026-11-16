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
static struct pd_calibration_reply configured(struct pd_calibration* s, unsigned n, unsigned spacing, int64_t now, int eligible)
{
    unsigned char bytes[11] = {'Q',0,0,0,0,0,0,0,(unsigned char)n,
        (unsigned char)(spacing>>8),(unsigned char)spacing};
    return pd_calibration_command(s,bytes,sizeof(bytes),now,eligible);
}
static void configured_measurements(void)
{
    const unsigned spacings[]={200,1001,1190,2610,10000};
    for (unsigned i=0;i<sizeof(spacings)/sizeof(spacings[0]);++i) {
        struct pd_calibration s={0};
        assert(configured(&s,1,spacings[i],100,1).applied);
        assert(s.phase==PD_CAL_ARMED && s.purpose==PD_SOURCE_MEASUREMENT && s.source_id==1);
        assert(s.tone_spacing_hz==spacings[i] && s.samples_left==192000);
        const int64_t deadline=s.deadline_ns;
        assert(!configured(&s,1,spacings[i],200,1).applied && s.deadline_ns==deadline);
        assert(configured(&s,1,spacings[i]==200?201:200,200,1).result==PD_CAL_REPLAY);
        assert(s.tone_spacing_hz==spacings[i] && s.source_id==1 && s.deadline_ns==deadline);
        assert(configured(&s,2,500,200,1).result==PD_CAL_BUSY);
        assert(configured(&s,2,500,300,1).result==PD_CAL_BUSY && s.tone_spacing_hz==spacings[i]);
        assert(pd_calibration_begin(&s,1000) && s.deadline_ns==INT64_C(8000001000));
        assert(configured(&s,3,600,1100,1).result==PD_CAL_BUSY && s.tone_spacing_hz==spacings[i]);
        pd_calibration_consume(&s,PD_CAL_SAMPLES);assert(s.phase==PD_CAL_COMPLETED);
        assert(command(&s,'M',4,2000,1).applied && pd_calibration_tone1(&s)==710 && pd_calibration_tone2(&s)==1900);
        assert(command(&s,'X',5,2001,1).applied);
        assert(configured(&s,6,spacings[i],2100,0).result==PD_CAL_BLOCKED && s.tone_spacing_hz==1190);
        assert(configured(&s,6,spacings[i],2200,1).result==PD_CAL_BLOCKED); // Receipt cannot become an arm.
        assert(configured(&s,7,spacings[i],2300,1).applied);
        assert(!pd_calibration_tick(&s,s.deadline_ns));
        assert(!configured(&s,7,spacings[i],2400,1).applied && s.phase==PD_CAL_EXPIRED);
        assert(command(&s,'A',8,2500,1).applied && s.tone_spacing_hz==1190);
    }
    struct pd_calibration s={0};
    const unsigned invalid[]={0,199,10001,65535};
    for (unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        assert(configured(&s,1,invalid[i],100,1).result==PD_CAL_INVALID && s.nonce==0);
    }
    unsigned char bytes[12]={'Q',0,0,0,0,0,0,0,1,3,232,0};
    for (size_t size=0;size<=sizeof(bytes);++size) {
        if (size!=11) { assert(pd_calibration_command(&s,bytes,size,100,1).result==PD_CAL_INVALID && s.nonce==0); }
    }
    assert(configured(&s,0,1000,100,1).result==PD_CAL_INVALID);
}
int main(void)
{
    configured_measurements();
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
    assert(command(&s, 'M', 11, 11000, 1).applied);
    assert(s.purpose == PD_SOURCE_MEASUREMENT && s.phase == PD_CAL_ARMED);
    deadline = s.deadline_ns;
    assert(!command(&s, 'M', 11, 12000, 1).applied && s.deadline_ns == deadline);
    assert(command(&s, 'A', 12, 12000, 1).result == PD_CAL_BUSY);
    assert(s.purpose == PD_SOURCE_MEASUREMENT && s.deadline_ns == deadline && s.source_id == 11);
    assert(pd_calibration_begin(&s, 13000));
    assert(s.deadline_ns == INT64_C(8000013000));
    pd_calibration_consume(&s, PD_CAL_SAMPLES);
    assert(s.phase == PD_CAL_COMPLETED && !pd_calibration_begin(&s, 14000));
    assert(!command(&s, 'M', 11, 14000, 1).applied && s.phase == PD_CAL_COMPLETED);
    assert(command(&s, 'M', 13, 15000, 0).result == PD_CAL_BLOCKED);
    assert(command(&s, 'M', 14, 15000, 1).applied);
    assert(!pd_calibration_tick(&s, s.deadline_ns) && s.phase == PD_CAL_EXPIRED);
    assert(command(&s, 'M', 15, 16000, 1).applied);
    assert(command(&s, 'X', 16, 17000, 1).applied && s.phase == PD_CAL_CANCELED);
    assert(command(&s, 'A', 17, 18000, 1).applied && s.purpose == PD_SOURCE_CALIBRATION);
    assert(command(&s, 'M', 18, 19000, 1).result == PD_CAL_BUSY);
    assert(s.purpose == PD_SOURCE_CALIBRATION);
    puts("bounded calibration timing/replay tests passed");
    return 0;
}
