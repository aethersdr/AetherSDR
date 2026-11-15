/* debug_alloc.h — vendored from codec2, non-embedded path only */
#ifndef DEBUG_ALLOC_H
#define DEBUG_ALLOC_H

#include <stdlib.h>

#define codec2_malloc(size)        (malloc(size))
#define codec2_calloc(nmemb, size) (calloc(nmemb, size))
#define codec2_free(ptr)           (free(ptr))

#define MALLOC(size)        codec2_malloc(size)
#define CALLOC(nmemb, size) codec2_calloc(nmemb, size)
#define FREE(ptr)           codec2_free(ptr)

#endif /* DEBUG_ALLOC_H */
