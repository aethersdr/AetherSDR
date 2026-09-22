#pragma once
#include <atomic>
#include <cstdlib>
#include <new>
namespace {
// Test-only replacement of ordinary C++ new/new[]. The acquisition-thread
// guard excludes control/pool preparation, so a vector/new regression inside
// processBlock() is observable even when it does not allocate through WDSP.
// This does not intercept malloc/realloc, aligned new, or private allocators;
// WDSP's own allocation guard remains responsible for its C allocation path.
thread_local bool inCallback = false;
thread_local std::size_t callbackAllocations = 0;
std::atomic<std::size_t> totalCallbackAllocations {0};

void* allocateForTest(std::size_t size)
{
    if (inCallback) { ++callbackAllocations; }
    for (;;) {
        if (void* memory = std::malloc(size == 0 ? 1 : size)) { return memory; }
        const std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) { throw std::bad_alloc(); }
        handler();
    }
}
} // namespace

void* operator new(std::size_t size) { return allocateForTest(size); }
void* operator new[](std::size_t size) { return allocateForTest(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try { return allocateForTest(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    try { return allocateForTest(size); } catch (...) { return nullptr; }
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
