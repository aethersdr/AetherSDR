/*
 * AetherSDR portability boundary for WDSP 2.00.
 *
 * Synchronization semantics mirror the small Win32 subset WDSP consumes.
 * Threads are detached because upstream discards their handles and coordinates
 * shutdown through its run flags and semaphores.
 */

#ifndef _WIN32

#include "wdsp_port.h"

#include <errno.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

enum WdspHandleKind
{
    WDSP_HANDLE_SEMAPHORE,
    WDSP_HANDLE_EVENT
};

struct WdspPortHandle
{
    enum WdspHandleKind kind;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    LONG count;
    LONG maximumCount;
    BOOL manualReset;
};

struct WdspThreadStart
{
    void (__cdecl *entry)(void*);
    void* context;
};

static _Atomic uint64_t g_allocationSequence = 0;
static _Atomic uint64_t g_outstandingAllocations = 0;
// AetherSDR patch 13: see wdspPortHandoffPauseForTest() below.
static _Atomic unsigned g_handoffPauseMicroseconds = 0;

static uint64_t monotonicMilliseconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static struct timespec realtimeDeadline(DWORD timeoutMs)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeoutMs / 1000U;
    deadline.tv_nsec += (long)(timeoutMs % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static HANDLE createWaitable(enum WdspHandleKind kind, LONG initialCount,
                             LONG maximumCount, BOOL manualReset)
{
    struct WdspPortHandle* handle = calloc(1, sizeof(*handle));
    if (handle == NULL)
    {
        return NULL;
    }

    handle->kind = kind;
    handle->count = initialCount;
    handle->maximumCount = maximumCount;
    handle->manualReset = manualReset;
    if (pthread_mutex_init(&handle->mutex, NULL) != 0 ||
        pthread_cond_init(&handle->condition, NULL) != 0)
    {
        pthread_mutex_destroy(&handle->mutex);
        free(handle);
        return NULL;
    }
    return handle;
}

void InitializeCriticalSection(CRITICAL_SECTION* section)
{
    InitializeCriticalSectionAndSpinCount(section, 0);
}

void InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION* section, DWORD spinCount)
{
    pthread_mutexattr_t attributes;
    (void)spinCount;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(section, &attributes);
    pthread_mutexattr_destroy(&attributes);
}

void EnterCriticalSection(CRITICAL_SECTION* section)
{
    pthread_mutex_lock(section);
}

void LeaveCriticalSection(CRITICAL_SECTION* section)
{
    pthread_mutex_unlock(section);
}

void DeleteCriticalSection(CRITICAL_SECTION* section)
{
    pthread_mutex_destroy(section);
}

HANDLE CreateSemaphore(void* attributes, LONG initialCount, LONG maximumCount, const char* name)
{
    (void)attributes;
    (void)name;
    if (initialCount < 0 || maximumCount <= 0 || initialCount > maximumCount)
    {
        return NULL;
    }
    return createWaitable(WDSP_HANDLE_SEMAPHORE, initialCount, maximumCount, FALSE);
}

HANDLE CreateSemaphoreW(void* attributes, LONG initialCount, LONG maximumCount, const void* name)
{
    return CreateSemaphore(attributes, initialCount, maximumCount, (const char*)name);
}

BOOL ReleaseSemaphore(HANDLE handle, LONG releaseCount, LONG* previousCount)
{
    if (handle == NULL || handle->kind != WDSP_HANDLE_SEMAPHORE || releaseCount <= 0)
    {
        return FALSE;
    }

    pthread_mutex_lock(&handle->mutex);
    if (previousCount != NULL)
    {
        *previousCount = handle->count;
    }
    if (handle->count > handle->maximumCount - releaseCount)
    {
        pthread_mutex_unlock(&handle->mutex);
        return FALSE;
    }
    handle->count += releaseCount;
    pthread_cond_broadcast(&handle->condition);
    pthread_mutex_unlock(&handle->mutex);
    return TRUE;
}

HANDLE CreateEvent(void* attributes, BOOL manualReset, BOOL initialState, const char* name)
{
    (void)attributes;
    (void)name;
    return createWaitable(WDSP_HANDLE_EVENT, initialState ? 1 : 0, 1, manualReset);
}

BOOL SetEvent(HANDLE handle)
{
    if (handle == NULL || handle->kind != WDSP_HANDLE_EVENT)
    {
        return FALSE;
    }
    pthread_mutex_lock(&handle->mutex);
    handle->count = 1;
    if (handle->manualReset)
    {
        pthread_cond_broadcast(&handle->condition);
    }
    else
    {
        pthread_cond_signal(&handle->condition);
    }
    pthread_mutex_unlock(&handle->mutex);
    return TRUE;
}

BOOL ResetEvent(HANDLE handle)
{
    if (handle == NULL || handle->kind != WDSP_HANDLE_EVENT)
    {
        return FALSE;
    }
    pthread_mutex_lock(&handle->mutex);
    handle->count = 0;
    pthread_mutex_unlock(&handle->mutex);
    return TRUE;
}

DWORD WaitForSingleObject(HANDLE handle, DWORD timeoutMs)
{
    int result = 0;
    if (handle == NULL)
    {
        return WAIT_FAILED;
    }

    pthread_mutex_lock(&handle->mutex);
    if (timeoutMs == 0 && handle->count == 0)
    {
        pthread_mutex_unlock(&handle->mutex);
        return WAIT_TIMEOUT;
    }

    if (timeoutMs == INFINITE)
    {
        while (handle->count == 0 && result == 0)
        {
            result = pthread_cond_wait(&handle->condition, &handle->mutex);
        }
    }
    else
    {
        const struct timespec deadline = realtimeDeadline(timeoutMs);
        while (handle->count == 0 && result == 0)
        {
            result = pthread_cond_timedwait(&handle->condition, &handle->mutex, &deadline);
        }
    }

    if (result != 0 || handle->count == 0)
    {
        pthread_mutex_unlock(&handle->mutex);
        return result == ETIMEDOUT ? WAIT_TIMEOUT : WAIT_FAILED;
    }

    if (handle->kind == WDSP_HANDLE_SEMAPHORE || !handle->manualReset)
    {
        --handle->count;
    }
    pthread_mutex_unlock(&handle->mutex);
    return WAIT_OBJECT_0;
}

DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL waitAll, DWORD timeoutMs)
{
    const uint64_t started = monotonicMilliseconds();
    if (handles == NULL || count == 0 || waitAll)
    {
        return WAIT_FAILED;
    }

    for (;;)
    {
        DWORD index;
        for (index = 0; index < count; ++index)
        {
            if (WaitForSingleObject(handles[index], 0) == WAIT_OBJECT_0)
            {
                return WAIT_OBJECT_0 + index;
            }
        }
        if (timeoutMs != INFINITE && monotonicMilliseconds() - started >= timeoutMs)
        {
            return WAIT_TIMEOUT;
        }
        Sleep(1);
    }
}

BOOL CloseHandle(HANDLE handle)
{
    if (handle == NULL)
    {
        return FALSE;
    }
    pthread_cond_destroy(&handle->condition);
    pthread_mutex_destroy(&handle->mutex);
    free(handle);
    return TRUE;
}

static void* threadTrampoline(void* context)
{
    struct WdspThreadStart* start = context;
    void (__cdecl *entry)(void*) = start->entry;
    void* entryContext = start->context;
    free(start);
    entry(entryContext);
    return NULL;
}

uintptr_t wdspBeginThread(void (__cdecl *entry)(void*), unsigned stackSize, void* context)
{
    pthread_t thread;
    pthread_attr_t attributes;
    struct WdspThreadStart* start = malloc(sizeof(*start));
    if (start == NULL)
    {
        return (uintptr_t)-1;
    }
    start->entry = entry;
    start->context = context;

    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (stackSize != 0)
    {
        pthread_attr_setstacksize(&attributes, stackSize);
    }
    if (pthread_create(&thread, &attributes, threadTrampoline, start) != 0)
    {
        pthread_attr_destroy(&attributes);
        free(start);
        return (uintptr_t)-1;
    }
    pthread_attr_destroy(&attributes);
    return (uintptr_t)thread;
}

void _endthread(void)
{
    pthread_exit(NULL);
}

BOOL QueueUserWorkItem(void* entry, void* context, DWORD flags)
{
    (void)flags;
    return wdspBeginThread((void (__cdecl *)(void*))entry, 0, context) != (uintptr_t)-1;
}

void Sleep(DWORD milliseconds)
{
    struct timespec requested = {
        .tv_sec = milliseconds / 1000U,
        .tv_nsec = (long)(milliseconds % 1000U) * 1000000L
    };
    while (nanosleep(&requested, &requested) != 0 && errno == EINTR)
    {
    }
}

HANDLE GetCurrentThread(void)
{
    return NULL;
}

BOOL SetThreadPriority(HANDLE thread, int priority)
{
    (void)thread;
    (void)priority;
    return TRUE;
}

HANDLE AvSetMmThreadCharacteristics(const char* taskName, DWORD* taskIndex)
{
    (void)taskName;
    (void)taskIndex;
    return NULL;
}

BOOL AvSetMmThreadPriority(HANDLE task, int priority)
{
    (void)task;
    (void)priority;
    return TRUE;
}

BOOL AvRevertMmThreadCharacteristics(HANDLE task)
{
    (void)task;
    return TRUE;
}

void* wdspAlignedAllocate(size_t size, size_t alignment)
{
    void* pointer = NULL;
    if (alignment < sizeof(void*))
    {
        alignment = sizeof(void*);
    }
    if (posix_memalign(&pointer, alignment, size) != 0)
    {
        return NULL;
    }
    atomic_fetch_add_explicit(&g_allocationSequence, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_outstandingAllocations, 1, memory_order_relaxed);
    return pointer;
}

void wdspAlignedFree(void* pointer)
{
    if (pointer != NULL)
    {
        atomic_fetch_sub_explicit(&g_outstandingAllocations, 1, memory_order_relaxed);
        free(pointer);
    }
}

uint64_t wdspPortAllocationSequence(void)
{
    return atomic_load_explicit(&g_allocationSequence, memory_order_relaxed);
}

uint64_t wdspPortOutstandingAllocations(void)
{
    return atomic_load_explicit(&g_outstandingAllocations, memory_order_relaxed);
}

// AetherSDR patch 13 (#5734). A test-only widening of the window between
// dexchange() releasing the host and dexchange() returning. Before patch 13 the
// worker still had to copy its input slot out of r1 in that window, so a host
// allowed to run on overwrote it; the pause makes that deterministic instead of
// a matter of CPU load. After patch 13 the copy is already done, and the pause
// only delays the worker. Nothing but wdsp_channel_test ever sets it.
void wdspPortSetHandoffPauseForTest(unsigned microseconds)
{
    atomic_store_explicit(&g_handoffPauseMicroseconds, microseconds, memory_order_relaxed);
}

void wdspPortHandoffPauseForTest(void)
{
    const unsigned microseconds =
        atomic_load_explicit(&g_handoffPauseMicroseconds, memory_order_relaxed);
    if (microseconds != 0)
    {
        struct timespec requested = {
            .tv_sec = microseconds / 1000000U,
            .tv_nsec = (long)(microseconds % 1000000U) * 1000L
        };
        while (nanosleep(&requested, &requested) != 0 && errno == EINTR)
        {
        }
    }
}

void wdspEnableFlushToZero(void)
{
#if defined(__aarch64__)
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24);
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__SSE__)
    unsigned int control;
    __asm__ volatile("stmxcsr %0" : "=m"(control));
    control |= (1U << 15);
    __asm__ volatile("ldmxcsr %0" : : "m"(control));
#endif
}

int wdspFreopen(FILE** stream, const char* filename, const char* mode, FILE* oldStream)
{
    FILE* result = freopen(filename, mode, oldStream);
    if (stream != NULL)
    {
        *stream = result;
    }
    return result == NULL ? errno : 0;
}

BOOL AllocConsole(void)
{
    return FALSE;
}

BOOL FreeConsole(void)
{
    return TRUE;
}

void OutputDebugStringA(const char* text)
{
    if (text != NULL)
    {
        fputs(text, stderr);
    }
}

#else

#include <malloc.h>
#include <stdint.h>
#include <Windows.h>

#undef _aligned_malloc
#undef _aligned_free

static volatile LONG64 g_allocationSequence = 0;
static volatile LONG64 g_outstandingAllocations = 0;

void* wdspAlignedAllocate(size_t size, size_t alignment)
{
    void* pointer = _aligned_malloc(size, alignment);
    if (pointer != NULL)
    {
        InterlockedIncrement64(&g_allocationSequence);
        InterlockedIncrement64(&g_outstandingAllocations);
    }
    return pointer;
}

void wdspAlignedFree(void* pointer)
{
    if (pointer != NULL)
    {
        InterlockedDecrement64(&g_outstandingAllocations);
        _aligned_free(pointer);
    }
}

uint64_t wdspPortAllocationSequence(void)
{
    return (uint64_t)InterlockedCompareExchange64(&g_allocationSequence, 0, 0);
}

uint64_t wdspPortOutstandingAllocations(void)
{
    return (uint64_t)InterlockedCompareExchange64(&g_outstandingAllocations, 0, 0);
}

// AetherSDR patch 13: the Windows twin of the POSIX pair above. Sleep() has
// millisecond resolution, so a non-zero request rounds UP to whole milliseconds
// -- a longer window, never a shorter one.
static volatile LONG g_handoffPauseMicroseconds = 0;

void wdspPortSetHandoffPauseForTest(unsigned microseconds)
{
    InterlockedExchange(&g_handoffPauseMicroseconds, (LONG)microseconds);
}

void wdspPortHandoffPauseForTest(void)
{
    const LONG microseconds = InterlockedCompareExchange(&g_handoffPauseMicroseconds, 0, 0);
    if (microseconds != 0)
    {
        Sleep((DWORD)((microseconds + 999) / 1000));
    }
}

#endif
