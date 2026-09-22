#include <aether_wdsp.h>
#include <cstddef>
#include <cstdio>
#include <thread>
extern "C" void* wdspAlignedAllocate(std::size_t, std::size_t);
extern "C" void wdspAlignedFree(void*);
int main()
{
    const auto local = wdspPortThreadAllocationSequence();
    const auto global = wdspPortAllocationSequence();
    const auto live = wdspPortOutstandingAllocations();
    std::thread preparation([] {
        void* memory = wdspAlignedAllocate(256, 64);
        wdspAlignedFree(memory);
    });
    preparation.join();
    if (wdspPortThreadAllocationSequence() != local || wdspPortAllocationSequence() != global + 1) {
        std::fprintf(stderr, "foreign-thread planning contaminates callback allocation guard\n"); return 1;
    }
    void* memory = wdspAlignedAllocate(256, 64);
    const bool detected = memory && wdspPortThreadAllocationSequence() == local + 1;
    wdspAlignedFree(memory);
    if (!detected || wdspPortOutstandingAllocations() != live) {
        std::fprintf(stderr, "local allocation was not detected or leaked\n"); return 1;
    }
    return 0;
}
