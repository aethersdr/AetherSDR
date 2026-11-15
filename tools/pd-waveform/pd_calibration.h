#pragma once
#include <stddef.h>
#include <stdint.h>

enum pd_calibration_phase {
    PD_CAL_IDLE, PD_CAL_ARMED, PD_CAL_RUNNING, PD_CAL_COMPLETED,
    PD_CAL_CANCELED, PD_CAL_EXPIRED
};
enum pd_calibration_result { PD_CAL_OK, PD_CAL_BUSY, PD_CAL_BLOCKED, PD_CAL_REPLAY, PD_CAL_INVALID };
enum { PD_CAL_SAMPLES = 8 * 24000 };
struct pd_calibration {
    enum pd_calibration_phase phase;
    int samples_left;
    int64_t deadline_ns, last_clock_ns;
    uint64_t nonce;
    unsigned char opcode, result;
};
struct pd_calibration_reply { enum pd_calibration_result result; int applied; };
// Fixed private commands: 'A' arm or 'X' cancel, followed by nonzero BE64 nonce.
// Duplicate requests return their original result without extending an arm.
struct pd_calibration_reply pd_calibration_command(struct pd_calibration* state,
    const unsigned char* bytes, size_t size, int64_t now_ns, int eligible);
int pd_calibration_begin(struct pd_calibration* state, int64_t now_ns);
int pd_calibration_tick(struct pd_calibration* state, int64_t now_ns);
void pd_calibration_consume(struct pd_calibration* state, unsigned samples);
void pd_calibration_cancel(struct pd_calibration* state);
const char* pd_calibration_name(const struct pd_calibration* state);
