#pragma once
#include <stddef.h>
#include <stdint.h>

enum { PD_CAPTURE_SAMPLES = 16384, PD_CAPTURE_CHUNK = 1024,
       PD_CAPTURE_HEADER = 96, PD_CAPTURE_REPLY = PD_CAPTURE_HEADER + 8 * PD_CAPTURE_CHUNK };
enum pd_capture_state { PD_CAPTURE_EMPTY, PD_CAPTURE_PENDING, PD_CAPTURE_READY,
                        PD_CAPTURE_INVALID, PD_CAPTURE_EXPIRED, PD_CAPTURE_UNAVAILABLE };
struct pd_capture_meta {
    uint64_t generation, center_hz;
    uint32_t stream, rate, keyed, correcting;
    uint64_t measurement_id;
    uint32_t measurement_tones, measurement_spacing_hz;
    uint32_t iq_channel, feedback_pan, gui_handle, binding_valid;
};
/* Owned exclusively by the DSP event loop. Samples are unchanged little-endian
 * complex float32 DAX IQ, before the correction feedback decimator/EQ. */
struct pd_capture {
    struct pd_capture_meta meta;
    uint64_t id;
    int64_t started_ns, first_ns, last_ns;
    enum pd_capture_state state;
    unsigned count;
    unsigned char samples[8 * PD_CAPTURE_SAMPLES];
};
void pd_capture_begin(struct pd_capture*, uint64_t id, const struct pd_capture_meta*, int64_t now);
void pd_capture_invalidate(struct pd_capture*);
void pd_capture_feed(struct pd_capture*, const struct pd_capture_meta*,
                     const unsigned char* samples, size_t bytes, int64_t now);
/* Fixed binary response; never returns partial or mismatched snapshot samples.
 * Completed snapshots remain readable after unkey, until the five-second TTL. */
size_t pd_capture_read(struct pd_capture*, uint64_t id, uint32_t offset, int64_t now,
                       unsigned char* output, size_t capacity);

enum { PD_CAPTURE_BATCH_MAX = 4, PD_CAPTURE_BATCH_HEADER = 104,
       PD_CAPTURE_BATCH_REPLY = PD_CAPTURE_BATCH_HEADER + 8 * PD_CAPTURE_CHUNK };
/* Fixed storage on the existing DSP loop. No acquisition waits for a reader.
 * One source, correction state and binding must survive the whole batch. */
struct pd_capture_batch {
    struct pd_capture frames[PD_CAPTURE_BATCH_MAX];
    uint64_t id;
    int64_t started_ns, last_ns;
    unsigned requested, completed;
    enum pd_capture_state state;
};
void pd_capture_batch_begin(struct pd_capture_batch*, uint64_t id, unsigned count,
                            const struct pd_capture_meta*, int64_t now);
void pd_capture_batch_invalidate(struct pd_capture_batch*);
void pd_capture_batch_feed(struct pd_capture_batch*, const struct pd_capture_meta*,
                           const unsigned char* samples, size_t bytes, int64_t now);
/* Offset is linear across frames. Header v5 appends frame index and count. */
size_t pd_capture_batch_read(struct pd_capture_batch*, uint64_t id, uint32_t offset,
                             int64_t now, unsigned char* output, size_t capacity);
