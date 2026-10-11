#pragma once

namespace AetherSDR {

// The Ulanzi Dial's HID identity, shared by the macOS and Windows backends.
// The #5126 Bluetooth LE descriptor dump records VendorID 65521 (0xFFF1) and
// ProductID 130 (0x0082); the bench device reports the same pair. They stay
// int because macOS passes their address to CFNumberCreate(kCFNumberIntType).
inline constexpr int kUlanziVendorId = 0xFFF1;
inline constexpr int kUlanziProductId = 0x0082;

} // namespace AetherSDR
