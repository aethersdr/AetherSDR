/*
 * AetherSDR portability boundary for the Windows-oriented WDSP sources.
 *
 * This header intentionally implements only the Win32 surface used by the
 * pinned WDSP snapshot. Keep platform policy here rather than scattering
 * conditional code through upstream files.
 */
#pragma once

#ifndef _WIN32

#include <pthread.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int BOOL;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef unsigned char byte;
typedef char* String;
typedef void* PVOID;
typedef pthread_mutex_t CRITICAL_SECTION;
typedef CRITICAL_SECTION* LPCRITICAL_SECTION;

typedef struct WdspPortHandle* HANDLE;

#define WINAPI
#define __cdecl
#define __stdcall
#define __forceinline inline
#define __declspec(value)
#define _int64 long long
#define TRUE 1
#define FALSE 0
#define TEXT(value) value
#define INFINITE UINT32_MAX
#define WAIT_OBJECT_0 0U
#define WAIT_TIMEOUT 258U
#define WAIT_FAILED UINT32_MAX
#define THREAD_PRIORITY_HIGHEST 0
#define WT_EXECUTEDEFAULT 0

#ifndef min
#define min(left, right) ((left) < (right) ? (left) : (right))
#endif
#ifndef max
#define max(left, right) ((left) > (right) ? (left) : (right))
#endif

// WDSP's diagnostic helper predates POSIX dprintf(int, ...). Rename it at the
// compatibility boundary after stdio has declared the POSIX function.
#define dprintf wdspDebugPrintf

#define InterlockedIncrement(base) __atomic_add_fetch((base), 1L, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(base) __atomic_sub_fetch((base), 1L, __ATOMIC_SEQ_CST)
#define InterlockedBitTestAndSet(base, bit) \
    __atomic_fetch_or((base), (1L << (bit)), __ATOMIC_SEQ_CST)
#define InterlockedBitTestAndReset(base, bit) \
    __atomic_fetch_and((base), ~(1L << (bit)), __ATOMIC_SEQ_CST)
#define InterlockedExchange(base, value) \
    __atomic_exchange_n((base), (value), __ATOMIC_SEQ_CST)
#define InterlockedAnd(base, mask) \
    __atomic_fetch_and((base), (mask), __ATOMIC_SEQ_CST)
#define _InterlockedAnd(base, mask) InterlockedAnd((base), (mask))

void InitializeCriticalSection(CRITICAL_SECTION* section);
void InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION* section, DWORD spinCount);
void EnterCriticalSection(CRITICAL_SECTION* section);
void LeaveCriticalSection(CRITICAL_SECTION* section);
void DeleteCriticalSection(CRITICAL_SECTION* section);

HANDLE CreateSemaphore(void* attributes, LONG initialCount, LONG maximumCount, const char* name);
HANDLE CreateSemaphoreW(void* attributes, LONG initialCount, LONG maximumCount, const void* name);
BOOL ReleaseSemaphore(HANDLE handle, LONG releaseCount, LONG* previousCount);
HANDLE CreateEvent(void* attributes, BOOL manualReset, BOOL initialState, const char* name);
BOOL SetEvent(HANDLE handle);
BOOL ResetEvent(HANDLE handle);
DWORD WaitForSingleObject(HANDLE handle, DWORD timeoutMs);
DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL waitAll, DWORD timeoutMs);
BOOL CloseHandle(HANDLE handle);

uintptr_t wdspBeginThread(void (__cdecl *entry)(void*), unsigned stackSize, void* context);
void _endthread(void);
BOOL QueueUserWorkItem(void* entry, void* context, DWORD flags);

#define _beginthread wdspBeginThread

void Sleep(DWORD milliseconds);
HANDLE GetCurrentThread(void);
BOOL SetThreadPriority(HANDLE thread, int priority);

HANDLE AvSetMmThreadCharacteristics(const char* taskName, DWORD* taskIndex);
BOOL AvSetMmThreadPriority(HANDLE task, int priority);
BOOL AvRevertMmThreadCharacteristics(HANDLE task);

void* wdspAlignedAllocate(size_t size, size_t alignment);
void wdspAlignedFree(void* pointer);
uint64_t wdspPortAllocationSequence(void);
uint64_t wdspPortOutstandingAllocations(void);

#define _aligned_malloc(size, alignment) wdspAlignedAllocate((size), (alignment))
#define _aligned_free(pointer) wdspAlignedFree((pointer))

void wdspEnableFlushToZero(void);
#define _MM_FLUSH_ZERO_ON 1
#define _MM_SET_FLUSH_ZERO_MODE(mode) do { (void)(mode); wdspEnableFlushToZero(); } while (0)

int wdspFreopen(FILE** stream, const char* filename, const char* mode, FILE* oldStream);
#define freopen_s(stream, filename, mode, oldStream) \
    wdspFreopen((stream), (filename), (mode), (oldStream))

BOOL AllocConsole(void);
BOOL FreeConsole(void);
void OutputDebugStringA(const char* text);

#ifdef __cplusplus
}
#endif

#endif
