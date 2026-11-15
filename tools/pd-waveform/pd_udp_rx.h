#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

int pd_udp_timestamp_enable(int fd);
/* The timestamp uses the legacy Linux ABI available on 2.6, including
 * 32-bit kernels used with a libc that has 64-bit time_t. Missing/truncated
 * metadata produces age=-1, so the TX guard rejects that datagram. */
ssize_t pd_udp_receive(int fd, unsigned char* packet, size_t capacity,
                       int64_t* arrival_ns, int64_t* age_ns);
