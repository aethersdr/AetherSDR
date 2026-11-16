#include "pd_capture.h"
#include "pd_calibration.h"
#include <string.h>

static const int64_t kLifetimeNs = 5000000000LL;
static const int64_t kCollectionNs = 1000000000LL;

static void put32(unsigned char* p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) { p[i] = (unsigned char)(value >> (24 - 8 * i)); }
}
static void put64(unsigned char* p, uint64_t value)
{
    put32(p, (uint32_t)(value >> 32)); put32(p + 4, (uint32_t)value);
}
static int same_meta(const struct pd_capture_meta* a, const struct pd_capture_meta* b)
{
    return a->generation == b->generation && a->center_hz == b->center_hz &&
        a->stream == b->stream && a->rate == b->rate &&
        a->keyed == b->keyed && a->correcting == b->correcting &&
        a->measurement_id == b->measurement_id && a->measurement_tones == b->measurement_tones &&
        a->measurement_spacing_hz == b->measurement_spacing_hz &&
        a->iq_channel == b->iq_channel && a->feedback_pan == b->feedback_pan &&
        a->gui_handle == b->gui_handle && a->binding_valid == b->binding_valid;
}
static void expire(struct pd_capture* c, int64_t now)
{
    if (c->state == PD_CAPTURE_PENDING || c->state == PD_CAPTURE_READY) {
        int64_t limit = c->state == PD_CAPTURE_PENDING ? kCollectionNs : kLifetimeNs;
        if (now < c->started_ns || now - c->started_ns > limit) {
            c->state = PD_CAPTURE_EXPIRED;
        }
    }
}
void pd_capture_begin(struct pd_capture* c, uint64_t id, const struct pd_capture_meta* m, int64_t now)
{
    /* Repeated/later-delivered requests cannot replace the owned snapshot. */
    if (!id || id <= c->id) { return; }
    c->id = id; c->meta = *m; c->started_ns = now;
    c->first_ns = c->last_ns = 0; c->count = 0;
    c->state = now > 0 && m->generation && m->center_hz && m->center_hz <= 1000000000 &&
        ((!m->iq_channel && !m->feedback_pan && !m->gui_handle && !m->binding_valid) ||
         (m->iq_channel >= 1 && m->iq_channel <= 4 && m->feedback_pan && m->gui_handle && m->binding_valid == 1)) &&
        m->stream && (m->rate == 24000 || m->rate == 48000 || m->rate == 96000 || m->rate == 192000) &&
        m->measurement_tones <= 1 && (!m->measurement_tones || (m->measurement_id && m->keyed)) &&
        (m->measurement_id ? (m->measurement_spacing_hz >= PD_TONE_MIN_SPACING_HZ &&
                             m->measurement_spacing_hz <= PD_TONE_MAX_SPACING_HZ) : !m->measurement_spacing_hz) &&
        m->keyed <= 1 && m->correcting <= m->keyed ? PD_CAPTURE_PENDING : PD_CAPTURE_UNAVAILABLE;
}
void pd_capture_invalidate(struct pd_capture* c)
{
    if (c->state == PD_CAPTURE_PENDING) { c->state = PD_CAPTURE_INVALID; }
}
void pd_capture_feed(struct pd_capture* c, const struct pd_capture_meta* m,
                     const unsigned char* samples, size_t bytes, int64_t now)
{
    expire(c, now);
    if (c->state != PD_CAPTURE_PENDING) { return; }
    if (!same_meta(&c->meta, m) || !samples || !bytes || bytes % 8 || bytes > 4096 ||
        now < c->last_ns || (c->count && now - c->last_ns > 100000000)) {
        pd_capture_invalidate(c); return;
    }
    /* Validate the whole datagram before accepting any part, including the
     * tail beyond the requested frame. IEEE float exponent 255 is nonfinite. */
    for (size_t i = 0; i < bytes; i += 4) {
        if ((samples[i + 3] & 0x7f) == 0x7f && (samples[i + 2] & 0x80)) {
            pd_capture_invalidate(c); return;
        }
    }
    size_t count = bytes / 8;
    if (count > PD_CAPTURE_SAMPLES - c->count) { count = PD_CAPTURE_SAMPLES - c->count; }
    memcpy(c->samples + 8 * c->count, samples, 8 * count);
    if (!c->count) { c->first_ns = now; }
    c->count += (unsigned)count; c->last_ns = now;
    if (c->count == PD_CAPTURE_SAMPLES) { c->state = PD_CAPTURE_READY; }
}
size_t pd_capture_read(struct pd_capture* c, uint64_t id, uint32_t offset, int64_t now,
                       unsigned char* out, size_t capacity)
{
    if (!out || capacity < PD_CAPTURE_REPLY) { return 0; }
    expire(c, now);
    memset(out, 0, PD_CAPTURE_HEADER);
    memcpy(out, "PDIQ0004", 8); put64(out + 8, id);
    enum pd_capture_state state = id && id == c->id ? c->state : PD_CAPTURE_UNAVAILABLE;
    if (offset >= PD_CAPTURE_SAMPLES || offset % PD_CAPTURE_CHUNK) { state = PD_CAPTURE_UNAVAILABLE; }
    put32(out + 16, (uint32_t)state); put32(out + 20, offset);
    if (state == PD_CAPTURE_UNAVAILABLE || state == PD_CAPTURE_EMPTY) { return PD_CAPTURE_HEADER; }
    put32(out + 28, c->meta.rate); put64(out + 32, c->meta.center_hz);
    put64(out + 40, c->meta.generation); put64(out + 48, (uint64_t)c->first_ns);
    put64(out + 56, (uint64_t)c->last_ns); put32(out + 64, c->meta.stream);
    put32(out + 68, c->meta.keyed | (c->meta.correcting << 1) | (c->meta.measurement_tones << 2));
    put64(out + 72, c->meta.measurement_id);
    put32(out + 80, c->meta.measurement_spacing_hz);
    put32(out + 84, c->meta.iq_channel); put32(out + 88, c->meta.feedback_pan);
    put32(out + 92, c->meta.gui_handle);
    if (state != PD_CAPTURE_READY) { return PD_CAPTURE_HEADER; }
    put32(out + 24, PD_CAPTURE_CHUNK);
    memcpy(out + PD_CAPTURE_HEADER, c->samples + 8 * offset, PD_CAPTURE_CHUNK * 8);
    return PD_CAPTURE_REPLY;
}

void pd_capture_batch_begin(struct pd_capture_batch* b, uint64_t id, unsigned count,
                            const struct pd_capture_meta* m, int64_t now)
{
    if (!id || id <= b->id) { return; }
    b->id = id; b->requested = count; b->completed = 0;
    b->started_ns = now; b->last_ns = 0; b->state = PD_CAPTURE_UNAVAILABLE;
    if (count < 1 || count > PD_CAPTURE_BATCH_MAX || !m->keyed ||
        !m->measurement_tones || !m->measurement_id || !m->binding_valid) { return; }
    for (unsigned i = 0; i < count; ++i) {
        b->frames[i].id = 0; b->frames[i].state = PD_CAPTURE_EMPTY;
    }
    pd_capture_begin(&b->frames[0], id, m, now);
    b->state = b->frames[0].state;
}

void pd_capture_batch_invalidate(struct pd_capture_batch* b)
{
    if (b->state == PD_CAPTURE_PENDING) { b->state = PD_CAPTURE_INVALID; }
}

void pd_capture_batch_feed(struct pd_capture_batch* b, const struct pd_capture_meta* m,
                           const unsigned char* samples, size_t bytes, int64_t now)
{
    if (b->state != PD_CAPTURE_PENDING) { return; }
    if (!samples || !bytes || bytes % 8 || bytes > 4096 || now < b->started_ns ||
        now - b->started_ns > kLifetimeNs || (b->last_ns &&
        (now < b->last_ns || now - b->last_ns > 100000000))) {
        pd_capture_batch_invalidate(b); return;
    }
    /* Validate even the tail beyond the last frame before publishing a batch. */
    for (size_t i = 0; i < bytes; i += 4) {
        if ((samples[i + 3] & 0x7f) == 0x7f && (samples[i + 2] & 0x80)) {
            pd_capture_batch_invalidate(b); return;
        }
    }
    b->last_ns = now;
    while (bytes && b->completed < b->requested) {
        struct pd_capture* c = &b->frames[b->completed];
        size_t take = (PD_CAPTURE_SAMPLES - c->count) * 8;
        if (take > bytes) { take = bytes; }
        pd_capture_feed(c, m, samples, take, now);
        if (c->state != PD_CAPTURE_PENDING && c->state != PD_CAPTURE_READY) {
            b->state = c->state; return;
        }
        samples += take; bytes -= take;
        if (c->state != PD_CAPTURE_READY) { break; }
        ++b->completed;
        if (b->completed == b->requested) {
            b->state = PD_CAPTURE_READY;
        } else {
            pd_capture_begin(&b->frames[b->completed], b->id, &b->frames[0].meta, now);
        }
    }
}

size_t pd_capture_batch_read(struct pd_capture_batch* b, uint64_t id, uint32_t offset,
                             int64_t now, unsigned char* out, size_t capacity)
{
    if (!out || capacity < PD_CAPTURE_BATCH_REPLY) { return 0; }
    if ((b->state == PD_CAPTURE_PENDING || b->state == PD_CAPTURE_READY) &&
        (now < b->started_ns || now - b->started_ns > kLifetimeNs)) {
        b->state = PD_CAPTURE_EXPIRED;
    }
    unsigned index = offset / PD_CAPTURE_SAMPLES;
    enum pd_capture_state state = id && id == b->id && b->requested >= 1 &&
        b->requested <= PD_CAPTURE_BATCH_MAX && index < b->requested &&
        !(offset % PD_CAPTURE_CHUNK) ? b->state : PD_CAPTURE_UNAVAILABLE;
    size_t size = PD_CAPTURE_BATCH_HEADER;
    memset(out, 0, PD_CAPTURE_BATCH_HEADER);
    if (state == PD_CAPTURE_PENDING || state == PD_CAPTURE_READY) {
        if (index <= b->completed && index < b->requested) {
            struct pd_capture* c = &b->frames[index];
            size_t legacy = pd_capture_read(c, id, offset % PD_CAPTURE_SAMPLES, now, out, capacity);
            if (c->state == PD_CAPTURE_EXPIRED) { b->state = PD_CAPTURE_EXPIRED; }
            state = c->state;
            if (legacy == PD_CAPTURE_REPLY) {
                memmove(out + PD_CAPTURE_BATCH_HEADER, out + PD_CAPTURE_HEADER,
                        8 * PD_CAPTURE_CHUNK);
                size = PD_CAPTURE_BATCH_REPLY;
            }
        } else {
            state = PD_CAPTURE_PENDING;
        }
    }
    memcpy(out, "PDIQ0005", 8); put64(out + 8, id);
    put32(out + 16, (uint32_t)state); put32(out + 20, offset);
    put32(out + 96, index); put32(out + 100, b->requested);
    return size;
}
