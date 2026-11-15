#include "pd_capture.h"
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
        a->keyed == b->keyed && a->correcting == b->correcting;
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
        m->stream && (m->rate == 24000 || m->rate == 48000 || m->rate == 96000 || m->rate == 192000) &&
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
    memcpy(out, "PDIQ0001", 8); put64(out + 8, id);
    enum pd_capture_state state = id && id == c->id ? c->state : PD_CAPTURE_UNAVAILABLE;
    if (offset >= PD_CAPTURE_SAMPLES || offset % PD_CAPTURE_CHUNK) { state = PD_CAPTURE_UNAVAILABLE; }
    put32(out + 16, (uint32_t)state); put32(out + 20, offset);
    if (state == PD_CAPTURE_UNAVAILABLE || state == PD_CAPTURE_EMPTY) { return PD_CAPTURE_HEADER; }
    put32(out + 28, c->meta.rate); put64(out + 32, c->meta.center_hz);
    put64(out + 40, c->meta.generation); put64(out + 48, (uint64_t)c->first_ns);
    put64(out + 56, (uint64_t)c->last_ns); put32(out + 64, c->meta.stream);
    put32(out + 68, c->meta.keyed | (c->meta.correcting << 1));
    if (state != PD_CAPTURE_READY) { return PD_CAPTURE_HEADER; }
    put32(out + 24, PD_CAPTURE_CHUNK);
    memcpy(out + PD_CAPTURE_HEADER, c->samples + 8 * offset, PD_CAPTURE_CHUNK * 8);
    return PD_CAPTURE_REPLY;
}
