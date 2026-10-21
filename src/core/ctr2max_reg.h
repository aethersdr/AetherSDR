// CTR2-Max registration: whether the unit's stock firmware has registered it.
//
// The stock firmware keeps its files in a LittleFS volume in the "spiffs"
// partition (0x310000, 0xE0000 bytes, 4096-byte blocks). Registering writes
// Reg.txt there; an unregistered unit has none. Since CTR2-Max firmware
// v1.02.00m it holds a 64-digit hash of the owner's call (65 bytes with its
// line ending), and the firmware rewrites an earlier plain-text call that way
// when it starts. Lynovation counts a unit as properly registered when Reg.txt
// is longer than CTR2MAX_REG_MIN_SIZE bytes, and so do we.
// AetherCTR2 runs only on a registered unit (agreed with Lynovation), and
// AetherSDR's installer checks the same before installing it, so both use
// this file.
//
// Read-only by construction: littlefs is built with LFS_READONLY, so no code
// that writes, erases or relocates blocks is compiled in.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CTR2MAX_REG_OFFSET     0x310000u
#define CTR2MAX_REG_SIZE       0xE0000u
#define CTR2MAX_REG_BLOCK      4096u
#define CTR2MAX_REG_MIN_SIZE   40u   // a registered Reg.txt is longer

// Reads len bytes at offset (from the start of the partition) into buf.
// Returns 0 on success.
typedef int (*ctr2max_reg_read_fn)(void *ctx, uint32_t offset, void *buf, uint32_t len);

typedef enum {
    CTR2MAX_REGISTERED = 1,
    CTR2MAX_NOT_REGISTERED = 0,
    CTR2MAX_REG_UNREADABLE = -1,   // no LittleFS volume there (erased, or not a CTR2-Max)
} ctr2max_reg_t;

// Mounts the volume read-only and looks for Reg.txt (any letter case) in its
// root, longer than CTR2MAX_REG_MIN_SIZE bytes. On CTR2MAX_REGISTERED, copies
// its first line (trimmed, the hashed call) into call when call_size > 0.
ctr2max_reg_t ctr2max_registration(ctr2max_reg_read_fn read, void *ctx, uint32_t size,
                                   char *call, size_t call_size);

#ifdef __cplusplus
}
#endif
