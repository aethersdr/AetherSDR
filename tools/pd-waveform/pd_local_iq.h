#pragma once
#include "pd_stream_guard.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum { PD_IQ_HEADER=56, PD_IQ_CONFIG=32, PD_IQ_MAX_PACKET=4096 };
struct pd_local_iq {
    int fd;
    struct pd_stream_guard guard;
    int64_t last_age_ns, max_age_seen_ns;
    int64_t last_receive_ns;
    int64_t last_valid_ns;
    uint64_t frames, stale, invalid, starts, duplicates, old, foreign;
};

void pd_iq_put32(unsigned char *p,uint32_t value);
void pd_iq_put64(unsigned char *p,uint64_t value);
uint32_t pd_iq_get32(const unsigned char *p);
uint64_t pd_iq_get64(const unsigned char *p);
int64_t pd_iq_monotonic_ns(void);
/* fd is a private inherited AF_UNIX datagram endpoint from the trusted launcher. */
int pd_local_iq_init(struct pd_local_iq *iq,int fd);
/* Returns 0 if no packet, 1 for accepted data, 2 if histories must be reset,
 * -1 on transport error. START data returns 2 with nonzero payload_bytes.
 * Other reset-worthy rejections return 2 with payload_bytes=0. */
int pd_local_iq_receive(struct pd_local_iq *iq,unsigned char *packet,size_t capacity,
                        size_t *payload_bytes);

/* Only before any data has been received. Discard bounded pre-API backlog
 * without parsing it or clearing any runtime fault/history evidence. */
int pd_local_iq_discard_startup_io(struct pd_local_iq *iq,
    ssize_t (*receive)(int,void*,size_t,int));
int pd_local_iq_discard_startup(struct pd_local_iq *iq);
