#include "pd_calibration.h"
#include <limits.h>

static const int64_t kArmNs = INT64_C(30000000000);
static const int64_t kRunNs = INT64_C(8000000000);
int pd_measurement_spacing_valid(unsigned spacing_hz)
{
    return spacing_hz >= PD_TONE_MIN_SPACING_HZ && spacing_hz <= PD_TONE_MAX_SPACING_HZ;
}
double pd_calibration_tone1(const struct pd_calibration* s)
{
    return PD_TONE_CENTER_HZ - s->tone_spacing_hz / 2.0;
}
double pd_calibration_tone2(const struct pd_calibration* s)
{
    return PD_TONE_CENTER_HZ + s->tone_spacing_hz / 2.0;
}
static int active(const struct pd_calibration* s)
{
    return s->phase == PD_CAL_ARMED || s->phase == PD_CAL_RUNNING;
}
int pd_calibration_tick(struct pd_calibration* s, int64_t now)
{
    if (!active(s)) { return 0; }
    if (now <= 0 || now < s->last_clock_ns || now >= s->deadline_ns) {
        s->phase = PD_CAL_EXPIRED; s->samples_left = 0; return 0;
    }
    s->last_clock_ns = now;
    return 1;
}
void pd_calibration_cancel(struct pd_calibration* s)
{
    if (active(s)) { s->phase = PD_CAL_CANCELED; s->samples_left = 0; }
}
struct pd_calibration_reply pd_calibration_command(struct pd_calibration* s,
    const unsigned char* bytes, size_t size, int64_t now, int eligible)
{
    struct pd_calibration_reply reply = {PD_CAL_INVALID, 0};
    if (!bytes || !size) { return reply; }
    unsigned spacing = 0;
    if (bytes[0] == 'Q') {
        if (size != 11) { return reply; }
        spacing = ((unsigned)bytes[9] << 8) | bytes[10];
        if (!pd_measurement_spacing_valid(spacing)) { return reply; }
    } else {
        if (size != 9 || (bytes[0] != 'A' && bytes[0] != 'M' && bytes[0] != 'X'
            && bytes[0] != '0' && bytes[0] != '1')) { return reply; }
        if (bytes[0] == 'A' || bytes[0] == 'M') { spacing = PD_TONE_DEFAULT_SPACING_HZ; }
    }
    uint64_t nonce = 0;
    for (unsigned i = 1; i < 9; ++i) { nonce = (nonce << 8) | bytes[i]; }
    if (!nonce) { return reply; }
    if (nonce == s->nonce && bytes[0] == s->opcode && spacing == s->request_spacing_hz) {
        reply.result = (enum pd_calibration_result)s->result; return reply;
    }
    if (nonce <= s->nonce) { reply.result = PD_CAL_REPLAY; return reply; }
    (void)pd_calibration_tick(s, now);
    reply.result = PD_CAL_OK;
    if (bytes[0] == 'X') { pd_calibration_cancel(s); reply.applied = 1; }
    else if (active(s)) { reply.result = PD_CAL_BUSY; }
    else if (!eligible || now <= 0 || now > INT64_MAX - kArmNs) { reply.result = PD_CAL_BLOCKED; }
    else if (bytes[0] == '0' || bytes[0] == '1') { reply.applied = 1; }
    else {
        s->phase = PD_CAL_ARMED; s->samples_left = PD_CAL_SAMPLES;
        s->source_id = nonce;
        s->purpose = bytes[0] == 'M' || bytes[0] == 'Q' ? PD_SOURCE_MEASUREMENT : PD_SOURCE_CALIBRATION;
        s->tone_spacing_hz = spacing;
        s->last_clock_ns = now; s->deadline_ns = now + kArmNs; reply.applied = 1;
    }
    s->nonce = nonce; s->opcode = bytes[0]; s->request_spacing_hz = spacing; s->result = (unsigned char)reply.result;
    return reply;
}
int pd_calibration_begin(struct pd_calibration* s, int64_t now)
{
    if (!pd_calibration_tick(s, now) || s->phase != PD_CAL_ARMED || now > INT64_MAX - kRunNs) { return 0; }
    s->phase = PD_CAL_RUNNING; s->last_clock_ns = now; s->deadline_ns = now + kRunNs;
    return 1;
}
void pd_calibration_consume(struct pd_calibration* s, unsigned samples)
{
    if (s->phase != PD_CAL_RUNNING) { return; }
    if (samples >= (unsigned)s->samples_left) { s->samples_left = 0; s->phase = PD_CAL_COMPLETED; }
    else { s->samples_left -= (int)samples; }
}
const char* pd_calibration_name(const struct pd_calibration* s)
{
    switch (s->phase) {
    case PD_CAL_ARMED: return "armed";
    case PD_CAL_RUNNING: return "running";
    case PD_CAL_COMPLETED: return "completed";
    case PD_CAL_CANCELED: return "canceled";
    case PD_CAL_EXPIRED: return "expired";
    default: return "idle";
    }
}
