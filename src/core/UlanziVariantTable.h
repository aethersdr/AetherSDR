#pragma once

#include <QString>

#include <cstddef>

namespace AetherSDR {
namespace UlanziVariant {

// Known Ulanzi OEM dial variants that enumerate over HID but CANNOT be driven
// by the native HID backends: their vendor collection stays silent unless the
// Ulanzi Studio app performs its activation handshake, and their keyboard and
// mouse collections are captured by the OS.  Recognising them lets the mapper
// say so instead of sitting on "Disconnected" forever.
//
// The trap that stalled #3485: the Windows PnP FriendlyName for these units IS
// "Ulanzi Dial" (the BLE GAP name), but hidapi reports the HID string
// descriptor, which is the OEM product string named below.
//
// Both rows pin the PID deliberately.  0xFFF1 is unassigned placeholder VID
// space that cheap BLE-HID firmware reuses freely, so a vendor-only match
// there would confidently mislabel unrelated hardware as a D100H — a worse
// failure than the "Disconnected" it replaces.  `pid == 0` stays available for
// a vendor later shown to ship one dial under several PIDs; if a row uses it,
// say why here.  (#3485)
struct KnownVariant {
    unsigned short vid;
    unsigned short pid;             // 0 = any (unused today — see note above)
    const char*    displayName;
};

inline constexpr KnownVariant kUnsupportedVariants[] = {
    {0xFFF1, 0x0082, "Ulanzi D100H (KEHWIN \"Dial_Lite\", BLE)"},
    {0x2207, 0x0019, "Ulanzi D200 (Zkswe \"ulanzi\", USB)"},
};

// Display name for a (vid, pid) that is a known-unsupported variant, or an
// empty string when it is not one.
inline QString lookup(unsigned short vid, unsigned short pid)
{
    for (const KnownVariant& v : kUnsupportedVariants) {
        if (vid == v.vid && (v.pid == 0 || pid == v.pid))
            return QString::fromUtf8(v.displayName);
    }
    return QString();
}

// Should a state change be published?  The advisory must fire in BOTH
// directions: the hotplug timer re-scans every few seconds, so an
// unconditional emit would spam the dialog, but suppressing the empty case
// would leave it claiming an unplugged dial is still present.  Emitting only
// on a genuine transition also means present -> absent -> same variant present
// notifies again rather than being deduped away.  (#3485)
inline bool shouldPublish(const QString& lastPublished, const QString& current)
{
    return lastPublished != current;
}

} // namespace UlanziVariant
} // namespace AetherSDR
