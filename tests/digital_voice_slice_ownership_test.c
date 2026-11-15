#include "digital_voice_slice_ownership.h"

#include <stdio.h>

int main(void)
{
    int failed = 0;
    digital_voice_slice_ownership ownership =
        DIGITAL_VOICE_SLICE_OWNERSHIP_INITIALIZER;

    digital_voice_slice_ownership_set(
        &ownership, 7U, TRUE, 0x12345678U);
    failed += digital_voice_slice_ownership_accepts(
        &ownership, 8U, TRUE, 0x12345678U, FALSE) != FALSE;
    failed += digital_voice_slice_ownership_accepts(
        &ownership, 7U, FALSE, 0U, FALSE) != TRUE;
    failed += digital_voice_slice_ownership_accepts(
        &ownership, 7U, TRUE, 0x87654321U, FALSE) != FALSE;
    failed += digital_voice_slice_ownership_accepts(
        &ownership, 7U, TRUE, 0x12345678U, FALSE) != TRUE;
    failed += digital_voice_slice_ownership_accepts(
        &ownership, 7U, TRUE, 0x12345678U, TRUE) != TRUE;
    failed += digital_voice_slice_ownership_accepts(
        &ownership, 7U, TRUE, 0x12345678U, FALSE) != FALSE;

    digital_voice_slice_ownership_set(&ownership, 3U, FALSE, 0U);
    failed += digital_voice_slice_ownership_accepts(
        &ownership, 3U, TRUE, 0x11111111U, FALSE) != TRUE;
    failed += digital_voice_slice_ownership_accepts(
        &ownership, 3U, TRUE, 0x22222222U, FALSE) != FALSE;

    printf("%s MultiFlex digital-voice slice ownership\n",
           failed == 0 ? "[ OK ]" : "[FAIL]");
    return failed == 0 ? 0 : 1;
}
