/* Opt-in C/Go protocol fixture. Only an inherited unnamed socket is used;
 * virtual packet timestamps exercise frame boundaries without radio or RF. */
#include "pd_capture.h"
#include "pd_status.h"
#include <poll.h>
#include <stdint.h>

static void sample32(unsigned char* p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) { p[i] = (unsigned char)(value >> (8 * i)); }
}

int main(void)
{
    if (pd_status_open("3") != 3) { return 1; }
    struct pd_capture_batch batch = {0};
    const struct pd_capture_meta meta = {7,7150000,0x40000001,48000,1,1,31,1,1190,3,0x40000002,7,1};
    const int64_t start = 1000000000;
    for (unsigned requests = 0; requests < 65; ++requests) {
        struct pollfd fd = {.fd = 3, .events = POLLIN};
        if (poll(&fd, 1, 2000) != 1) { return 2; }
        struct pd_status_query q;
        if (pd_status_query(3, &q) != 1) { return 3; }
        if (q.operation == 'G' && !batch.id) {
            pd_capture_batch_begin(&batch, q.capture_id, q.capture_offset, &meta, start);
        } else if (q.operation != 'H' || !batch.id) { return 4; }
        unsigned char reply[PD_CAPTURE_BATCH_REPLY];
        uint32_t offset = q.operation == 'G' ? 0 : q.capture_offset;
        int64_t now = q.operation == 'G' ? start : start + 2000000000;
        size_t size = pd_capture_batch_read(&batch, q.capture_id, offset, now, reply, sizeof(reply));
        if (!pd_status_reply(3, q.nonce, (const char*)reply, size)) { return 5; }
        if (q.operation == 'G') {
            /* 375 does not divide a frame: packet tails must cross frames. */
            for (unsigned index = 0; index < batch.requested * PD_CAPTURE_SAMPLES; index += 375) {
                unsigned char samples[375 * 8];
                for (unsigned i = 0; i < 375; ++i) {
                    sample32(samples + 8 * i, 0x3e000000 + index + i);
                    sample32(samples + 8 * i + 4, 0xbe000000 + index + i);
                }
                pd_capture_batch_feed(&batch, &meta, samples, sizeof(samples),
                                      start + (int64_t)(index + 375) * 1000000000 / meta.rate);
            }
            if (batch.completed != batch.requested || batch.state != PD_CAPTURE_READY) { return 6; }
            pd_capture_batch_invalidate(&batch);
        }
        if (q.operation == 'H' && offset == batch.requested * PD_CAPTURE_SAMPLES - PD_CAPTURE_CHUNK) { return 0; }
    }
    return 7;
}
