// Exercise the production discovery facade against HID symbols in this
// executable: no HID hardware or sockets. The same churn runs under TSan.
#include "core/HidDiscovery.h"

#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <thread>
#include <vector>

namespace {

std::atomic<int> g_active{0};
std::atomic<int> g_overlaps{0};
std::atomic<int> g_errors{0};
wchar_t g_error[64] = L"";
bool g_reenter = false;

struct Operation {
    Operation()
    {
        if (g_active.fetch_add(1) != 0) {
            ++g_overlaps;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    ~Operation() { --g_active; }
};

} // namespace

extern "C" {

int HID_API_CALL hid_init()
{
    const Operation operation;
    return 0;
}

hid_device_info* HID_API_CALL hid_enumerate(unsigned short, unsigned short)
{
    if (g_reenter) {
        g_reenter = false;
        // macOS enumeration processes the calling thread's run loop.
        AetherSDR::HidDiscovery::init();
    }
    const Operation operation;
    std::wcscpy(g_error, L"enumeration error");
    return nullptr;
}

hid_device* HID_API_CALL hid_open(unsigned short, unsigned short, const wchar_t*)
{
    const Operation operation;
    std::wcscpy(g_error, L"open error");
    return nullptr;
}

hid_device* HID_API_CALL hid_open_path(const char*)
{
    const Operation operation;
    std::wcscpy(g_error, L"path error");
    errno = EACCES;
    return nullptr;
}

const wchar_t* HID_API_CALL hid_error(hid_device*)
{
    const Operation operation;
    // Error access can disturb errno; the facade must preserve the open's.
    errno = EINVAL;
    return g_error;
}

} // extern "C"

int main()
{
    constexpr int kWorkers = 4;
    constexpr int kIterations = 100;
    std::barrier start(kWorkers);
    std::vector<std::thread> workers;
    for (int n = 0; n < kWorkers; ++n) {
        workers.emplace_back([&start] {
            start.arrive_and_wait();
            for (int i = 0; i < kIterations; ++i) {
                AetherSDR::HidDiscovery::init();
                AetherSDR::HidDiscovery::enumerate(0xffff, 0xffff);
                AetherSDR::HidDiscovery::open(0xffff, 0xffff, nullptr);
                std::wstring error;
                if (AetherSDR::HidDiscovery::openPath("absent", &error) != nullptr
                    || error != L"path error" || errno != EACCES) {
                    ++g_errors;
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    g_reenter = true;
    AetherSDR::HidDiscovery::enumerate(0xffff, 0xffff);
    if (g_overlaps != 0 || g_errors != 0 || g_reenter) {
        std::fprintf(stderr, "FAIL: overlaps=%d, wrong errors=%d, reentry=%d\n",
                     g_overlaps.load(), g_errors.load(), g_reenter);
        return 1;
    }
    std::puts("PASS: discovery/open serialized; error copy, errno and reentry preserved");
}
