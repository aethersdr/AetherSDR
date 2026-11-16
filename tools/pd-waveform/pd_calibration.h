#pragma once
#include <stddef.h>
#include <stdint.h>

enum pd_calibration_phase {
    PD_CAL_IDLE, PD_CAL_ARMED, PD_CAL_RUNNING, PD_CAL_COMPLETED,
    PD_CAL_CANCELED, PD_CAL_EXPIRED
};
enum pd_calibration_result { PD_CAL_OK, PD_CAL_BUSY, PD_CAL_BLOCKED, PD_CAL_REPLAY, PD_CAL_INVALID };
enum pd_source_purpose { PD_SOURCE_CALIBRATION, PD_SOURCE_MEASUREMENT };
enum {
    PD_CAL_SAMPLES = 8 * 24000,
    PD_TONE_MIN_SPACING_HZ = 200, PD_TONE_MAX_SPACING_HZ = 10000,
    PD_TONE_DEFAULT_SPACING_HZ = 1190, PD_TONE_CENTER_HZ = 1305
};
struct pd_calibration {
    enum pd_calibration_phase phase;
    enum pd_source_purpose purpose;
    int samples_left;
    int64_t deadline_ns, last_clock_ns;
    uint64_t nonce, source_id;
    unsigned tone_spacing_hz, request_spacing_hz;
    unsigned char opcode, result;
};
struct pd_calibration_reply { enum pd_calibration_result result; int applied; };
// Fixed commands: 'A' calibrate, 'M' measure, 'X' cancel, '0' bypass or '1' correct,
// then BE64 nonce. Correction selection never arms a source or extends its life.
// Measurement selects the same bounded tones without invalidating the PA model.
// Duplicate requests return their original result without extending an arm.
// 'Q' + BE64 nonce + BE16 spacing explicitly arms configured measurement tones.
// Its immutable pair is centered at1305Hz; legacy A/M retain710/1900Hz.
struct pd_calibration_reply pd_calibration_command(struct pd_calibration* state,
    const unsigned char* bytes, size_t size, int64_t now_ns, int eligible);
int pd_calibration_begin(struct pd_calibration* state, int64_t now_ns);
int pd_calibration_tick(struct pd_calibration* state, int64_t now_ns);
void pd_calibration_consume(struct pd_calibration* state, unsigned samples);
void pd_calibration_cancel(struct pd_calibration* state);
const char* pd_calibration_name(const struct pd_calibration* state);
int pd_measurement_spacing_valid(unsigned spacing_hz);
double pd_calibration_tone1(const struct pd_calibration* state);
double pd_calibration_tone2(const struct pd_calibration* state);
