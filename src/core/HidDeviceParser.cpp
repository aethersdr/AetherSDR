#ifdef HAVE_HIDAPI
#include "HidDeviceParser.h"

namespace AetherSDR {

static const HidDeviceId kSupportedDevices[] = {
    {0x0C26, 0x001E, "Icom RC-28"},
    {0x077D, 0x0410, "Griffin PowerMate"},
    {0x0B33, 0x0020, "Contour ShuttleXpress"},
    {0x0B33, 0x0030, "Contour ShuttlePro v2"},
};

const HidDeviceId* HidDeviceParser::supportedDevices() { return kSupportedDevices; }
int HidDeviceParser::supportedDeviceCount() { return static_cast<int>(std::size(kSupportedDevices)); }

std::unique_ptr<HidDeviceParser> HidDeviceParser::create(uint16_t vid, uint16_t pid)
{
    if (vid == 0x0C26 && pid == 0x001E) return std::make_unique<IcomRC28Parser>();
    if (vid == 0x077D && pid == 0x0410) return std::make_unique<GriffinPowerMateParser>();
    if (vid == 0x0B33 && pid == 0x0020) return std::make_unique<ShuttleXpressParser>();
    if (vid == 0x0B33 && pid == 0x0030) return std::make_unique<ShuttleProV2Parser>();
    return nullptr;
}

// ── Icom RC-28 ──────────────────────────────────────────────────────────────
// 32-byte reports. hidapi prepends the 1-byte report ID on Windows/Linux so
// hid_read(buf, 33) returns 33; on macOS it strips the ID and returns 32.
// We request 33 bytes (reportSize) and use len to tell the two apart — no
// value-based heuristic that could collide with data bytes.
// Data layout (0-indexed, after stripping report ID where present):
//   [0] seq counter: 0x01 = first report of knob step, 0x02 = second
//   [1] unused (0x00)
//   [2] direction: 0x01 = CW (+1), 0x02 = CCW (-1), 0x00 = no rotation
//   [3] unused (0x00)
//   [4] button state enum: 0x07 = idle, 0x05 = F1, 0x03 = F2, 0x06 = TX bar
// Each knob detent fires two reports (seq 0x01 then 0x02); we emit only on
// seq 0x01 to produce exactly one tuning step per detent.

HidEvent IcomRC28Parser::parse(const uint8_t* buf, size_t len)
{
    if (len < 5) return {};

    // Windows/Linux: hidapi includes the report ID → hid_read returns 33 bytes.
    // macOS: hidapi strips the report ID → hid_read returns 32 bytes.
    const uint8_t* data = (len >= 33) ? buf + 1 : buf;

    const uint8_t seq  = data[0];
    const uint8_t dir  = data[2];
    const uint8_t btns = data[4];

    // Button events use absolute state, not bitmask.
    // RC-28 has 3 buttons: F1 (1), F2 (2), TX bar (3).
    if (btns != m_prevButtonState) {
        const uint8_t prev = m_prevButtonState;
        m_prevButtonState  = btns;
        if (btns == 0x07) {
            // Release — identify which button was just released.
            if (prev == 0x05) return {HidEvent::Button, 0, 1, 1};  // F1
            if (prev == 0x03) return {HidEvent::Button, 0, 2, 1};  // F2
            if (prev == 0x06) return {HidEvent::Button, 0, 3, 1};  // TX bar
        } else {
            if (btns == 0x05) return {HidEvent::Button, 0, 1, 0};  // F1
            if (btns == 0x03) return {HidEvent::Button, 0, 2, 0};  // F2
            if (btns == 0x06) return {HidEvent::Button, 0, 3, 0};  // TX bar
        }
    }

    // Rotation: only emit on first report of each pair (seq == 0x01) to
    // produce exactly one step per detent (the device sends two reports per click).
    if (seq == 0x01 && dir != 0x00) {
        return {HidEvent::Rotate, (dir == 0x01) ? 1 : -1, 0, 0};
    }

    return {};
}

// ── Griffin PowerMate ───────────────────────────────────────────────────────
// 6-byte reports. Byte 0 = button (0/1). Byte 1 = signed rotation delta.

HidEvent GriffinPowerMateParser::parse(const uint8_t* buf, size_t len)
{
    if (len < 2) return {};

    uint8_t btn = buf[0];
    auto rot = static_cast<int8_t>(buf[1]);

    // Button change
    if (btn != m_prevButton) {
        m_prevButton = btn;
        return {HidEvent::Button, 0, 1, btn ? 0 : 1};
    }

    // Rotation
    if (rot != 0)
        return {HidEvent::Rotate, static_cast<int>(rot), 0, 0};

    return {};
}

// ── Contour ShuttleXpress ───────────────────────────────────────────────────
// 5-byte reports. Byte 0 = shuttle position (signed, -7..+7).
// Byte 1 = jog counter (wrapping uint8). Bytes 2-3 = button bitmask (5 btns).

HidEvent ShuttleXpressParser::parse(const uint8_t* buf, size_t len)
{
    if (len < 4) return {};

    uint8_t jog = buf[1];
    uint8_t btns = buf[3];

    // Buttons
    if (btns != m_prevButtons) {
        for (int b = 0; b < 5; ++b) {
            uint8_t mask = 1 << b;
            if ((btns & mask) != (m_prevButtons & mask)) {
                m_prevButtons = btns;
                return {HidEvent::Button, 0, b + 1, (btns & mask) ? 0 : 1};
            }
        }
        m_prevButtons = btns;
    }

    // Jog wheel (relative, wrapping)
    if (m_firstReport) {
        m_firstReport = false;
        m_prevJog = jog;
        return {};
    }

    if (jog != m_prevJog) {
        int delta = static_cast<int>(jog) - static_cast<int>(m_prevJog);
        if (delta > 128) delta -= 256;
        if (delta < -128) delta += 256;
        m_prevJog = jog;
        return {HidEvent::Rotate, delta, 0, 0};
    }

    return {};
}

// ── Contour ShuttlePro v2 ──────────────────────────────────────────────────
// Same layout as ShuttleXpress but 15 buttons across bytes 2-3.

HidEvent ShuttleProV2Parser::parse(const uint8_t* buf, size_t len)
{
    if (len < 4) return {};

    uint8_t jog = buf[1];
    uint16_t btns = static_cast<uint16_t>(buf[3] << 8 | buf[2]);

    // Buttons
    if (btns != m_prevButtons) {
        for (int b = 0; b < 15; ++b) {
            uint16_t mask = 1 << b;
            if ((btns & mask) != (m_prevButtons & mask)) {
                m_prevButtons = btns;
                return {HidEvent::Button, 0, b + 1, (btns & mask) ? 0 : 1};
            }
        }
        m_prevButtons = btns;
    }

    // Jog wheel
    if (m_firstReport) {
        m_firstReport = false;
        m_prevJog = jog;
        return {};
    }

    if (jog != m_prevJog) {
        int delta = static_cast<int>(jog) - static_cast<int>(m_prevJog);
        if (delta > 128) delta -= 256;
        if (delta < -128) delta += 256;
        m_prevJog = jog;
        return {HidEvent::Rotate, delta, 0, 0};
    }

    return {};
}

} // namespace AetherSDR
#endif
