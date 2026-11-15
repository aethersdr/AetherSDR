#include "HidDiscovery.h"

#include <cerrno>
#include <mutex>

namespace AetherSDR::HidDiscovery {
namespace {

// macOS HIDAPI 0.15 clears a shared error string even in implicit hid_init()
// calls, and enumeration changes a shared IOHIDManager's matching filter.
// Its run-loop processing can reenter discovery on the calling thread.
std::recursive_mutex& discoveryMutex()
{
    static std::recursive_mutex mutex;
    return mutex;
}

} // namespace

int init()
{
    const std::lock_guard<std::recursive_mutex> lock(discoveryMutex());
    // Keep HIDAPI alive for the process: destroying one controller cannot
    // establish that every other controller and detached I/O port has closed.
    return hid_init();
}

hid_device_info* enumerate(unsigned short vendorId, unsigned short productId)
{
    const std::lock_guard<std::recursive_mutex> lock(discoveryMutex());
    return hid_enumerate(vendorId, productId);
}

hid_device* open(unsigned short vendorId, unsigned short productId,
                 const wchar_t* serialNumber)
{
    const std::lock_guard<std::recursive_mutex> lock(discoveryMutex());
    return hid_open(vendorId, productId, serialNumber);
}

hid_device* openPath(const char* path, std::wstring* error)
{
    const std::lock_guard<std::recursive_mutex> lock(discoveryMutex());
    hid_device* device = hid_open_path(path);
    const int openErrno = errno;
    if (error) {
        const wchar_t* message = device ? nullptr : hid_error(nullptr);
        *error = message ? message : L"";
    }
    errno = openErrno;
    return device;
}

} // namespace AetherSDR::HidDiscovery
