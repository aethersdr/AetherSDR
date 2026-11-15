#pragma once
#include <stddef.h>
#include <stdint.h>

enum pd_guard_result {
    PD_GUARD_ACCEPT,
    PD_GUARD_START,
    PD_GUARD_STALE,
    PD_GUARD_DUPLICATE,
    PD_GUARD_OLD,
    PD_GUARD_INVALID,
    PD_GUARD_FOREIGN
};

struct pd_guard_frame {
    const unsigned char *payload;
    unsigned complex_samples;
    uint32_t seconds;
    uint64_t picoseconds;
};

struct pd_stream_guard {
    uint32_t sid;
    uint64_t generation;
    unsigned rate;
    int64_t max_age_ns;
    uint32_t last_seconds;
    uint64_t last_picoseconds;
    unsigned last_samples;
    unsigned last_count;
    int seen;
    int reset_pending;
    uint64_t accepted;
    uint64_t discontinuities;
};

/* A START frame requires clearing paired histories and reacquiring alignment.
 * The caller supplies measured packet age, not an FPGA/CPU clock subtraction.
 * The guard allocates nothing and never changes TX state. */
int pd_guard_init(struct pd_stream_guard *g, uint32_t sid, uint64_t generation,
                  unsigned rate, int64_t max_age_ns);
enum pd_guard_result pd_guard_check(struct pd_stream_guard *g, uint64_t generation,
                                    const unsigned char *packet, size_t bytes,
                                    int64_t age_ns, struct pd_guard_frame *frame);
