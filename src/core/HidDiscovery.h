#pragma once

#include <hidapi/hidapi.h>

#include <string>

namespace AetherSDR::HidDiscovery {

// Discovery/open calls share HIDAPI's process-wide manager and error string.
// Device I/O and close remain owned by each controller's I/O thread.
int init();
hid_device_info* enumerate(unsigned short vendorId, unsigned short productId);
hid_device* open(unsigned short vendorId, unsigned short productId,
                 const wchar_t* serialNumber);
hid_device* openPath(const char* path, std::wstring* error = nullptr);

} // namespace AetherSDR::HidDiscovery
