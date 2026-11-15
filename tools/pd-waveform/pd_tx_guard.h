#pragma once
#include <stddef.h>
#include <stdint.h>

enum pd_tx_result { PD_TX_ACCEPT,PD_TX_START,PD_TX_DROP,PD_TX_INVALID,PD_TX_STALE,PD_TX_FOREIGN };
struct pd_tx_guard {
    uint32_t sid,seconds;
    uint64_t generation,picoseconds;
    int64_t last_arrival_ns,max_age_ns;
    unsigned count,samples;
    int seen,reset_pending;
    uint64_t discontinuities,dropped;
};
int pd_tx_guard_init(struct pd_tx_guard *g,uint32_t sid,uint64_t generation,int64_t max_age_ns);
/* Arrival time is the local kernel receive timestamp, not the FPGA clock.
 * A long interval is conservatively treated as lost continuity. Source
 * timestamps are compared for duplicates/reordering, not assumed sample clocks. */
enum pd_tx_result pd_tx_guard_check(struct pd_tx_guard *g,uint64_t generation,
                                   const unsigned char *p,size_t bytes,
                                   int64_t arrival_ns,int64_t age_ns);
