#pragma once
#include <stddef.h>
#include <stdint.h>

// Optional inherited private Unix datagram socket. -1 absent, -2 invalid.
int pd_status_open(const char* descriptor);
// Exactly '?' and an eight-byte nonce are accepted; no control operations.
int pd_status_request(int fd, unsigned char nonce[8]);
struct pd_status_query {
    unsigned char operation, nonce[8];
    uint64_t capture_id;
    uint32_t capture_offset;
};
// '?' status, 'B' begin IQ snapshot, 'R' read snapshot. No radio controls.
int pd_status_query(int fd, struct pd_status_query*);
int pd_status_reply(int fd, const unsigned char nonce[8], const char* json, size_t size);
