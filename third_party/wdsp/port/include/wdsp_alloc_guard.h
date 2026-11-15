/* Force-included for WDSP translation units on Windows. */
#pragma once

#ifdef _WIN32

#include <stddef.h>
#include <malloc.h>

#ifdef __cplusplus
extern "C" {
#endif

void* wdspAlignedAllocate(size_t size, size_t alignment);
void wdspAlignedFree(void* pointer);

#ifdef __cplusplus
}
#endif

#define _aligned_malloc(size, alignment) wdspAlignedAllocate((size), (alignment))
#define _aligned_free(pointer) wdspAlignedFree((pointer))

#endif
