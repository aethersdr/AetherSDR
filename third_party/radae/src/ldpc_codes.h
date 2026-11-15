/*
  FILE...: ldpc_codes.h
  Minimal LDPC codes table — HRA_56_56 only, for rade_text EOO callsign.
*/

#ifndef __LDPC_CODES__
#define __LDPC_CODES__

#ifdef __cplusplus
extern "C" {
#endif

#include "mpdecode_core.h"

extern struct LDPC ldpc_codes[];
int ldpc_codes_num(void);
int ldpc_codes_find(char name[]);

#ifdef __cplusplus
}
#endif

#endif /* __LDPC_CODES__ */
