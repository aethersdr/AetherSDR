/*
  FILE...: ldpc_codes.c
  Minimal LDPC codes table — HRA_56_56 only, for rade_text EOO callsign.
  Vendored from codec2 and trimmed to the single code we need.
*/

#include "ldpc_codes.h"
#include "HRA_56_56.h"

#include <assert.h>
#include <string.h>

struct LDPC ldpc_codes[] = {
    {"HRA_56_56",
     HRA_56_56_MAX_ITER, 0, 1, 1,
     HRA_56_56_CODELENGTH,
     HRA_56_56_NUMBERPARITYBITS,
     HRA_56_56_NUMBERROWSHCOLS,
     HRA_56_56_MAX_ROW_WEIGHT,
     HRA_56_56_MAX_COL_WEIGHT,
     (uint16_t *)HRA_56_56_H_rows,
     (uint16_t *)HRA_56_56_H_cols},
};

int ldpc_codes_num(void) {
    return (int)(sizeof(ldpc_codes) / sizeof(ldpc_codes[0]));
}

int ldpc_codes_find(char name[]) {
    for (int c = 0; c < ldpc_codes_num(); c++)
        if (strcmp(ldpc_codes[c].name, name) == 0) return c;
    assert(0 && "LDPC code not found");
    return -1;
}
