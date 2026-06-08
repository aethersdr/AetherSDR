#ifdef HAVE_HIDAPI
#include "HidDeviceParser.h"

namespace AetherSDR {

static const HidDeviceId kSupportedDevices[] = {
    {0x0C26, 0x001E, "Icom RC-28 Remote Encoder"},
    {0x077D, 0x0410, "Griffin PowerMate"},
    {0x0B33, 0x0020, "Contour ShuttleXpress"},
    {0x0B33, 0x0030, "Contour ShuttlePro v2"},
    // AetherPad (Arduino Giga R1) running its RC-28 emulator firmware
    // (github.com/nigelfenton/aether-pad). The Giga's mbed PluggableUSB
    // stack hardcodes the device-level VID/PID to Arduino's composite
    // 0x2341:0x0266, so the emulator can't claim Icom's identity — we
    // route the Arduino-default VID/PID to IcomRC28Parser instead.
    // Gives a hardware-in-the-loop test bench for the RC-28 wire
    // protocol; validated against PR #2870's report-layout fix
    // (end-to-end test 2026-05-20).
    {0x2341, 0x0266, "AetherPad RC-28 emulator (Arduino Giga R1)"},
    // Elgato StreamDeck+ — 8 LCD buttons + 4 encoder dials (#1510)
    {0x0FD9, 0x0084, "Elgato StreamDeck+"},
    // ELAD/WoodBoxRadio TMate 2 — 3 encoders, 9 keys, RGB LCD backlight.
    // Protocol reverse-engineered via USBPcap; documented in OpenTMate2Lib.
    {0x1721, 0x0614, "ELAD/WoodBoxRadio TMate 2"},
};

const HidDeviceId* HidDeviceParser::supportedDevices() { return kSupportedDevices; }
int HidDeviceParser::supportedDeviceCount() { return static_cast<int>(std::size(kSupportedDevices)); }

std::unique_ptr<HidDeviceParser> HidDeviceParser::create(uint16_t vid, uint16_t pid)
{
    if (vid == 0x0C26 && pid == 0x001E) return std::make_unique<IcomRC28Parser>();
    if (vid == 0x077D && pid == 0x0410) return std::make_unique<GriffinPowerMateParser>();
    if (vid == 0x0B33 && pid == 0x0020) return std::make_unique<ShuttleXpressParser>();
    if (vid == 0x0B33 && pid == 0x0030) return std::make_unique<ShuttleProV2Parser>();
    // AetherPad emulator alias — same parser as the real RC-28 (see
    // comment in kSupportedDevices above).
    if (vid == 0x2341 && pid == 0x0266) return std::make_unique<IcomRC28Parser>();
    if (vid == 0x0FD9 && pid == 0x0084) return std::make_unique<StreamDeckPlusParser>();
    if (vid == 0x1721 && pid == 0x0614) return std::make_unique<TMate2Parser>();
    return nullptr;
}

// ── Icom RC-28 ──────────────────────────────────────────────────────────────
// 32-byte reports, no report ID prefix on any platform (hidraw returns 32 bytes).
// Verified layout (cross-referenced against FlexRC-28 driver and wfview source):
//   [0] = 0x01 constant — guard byte, discard report if != 0x01
//   [1] = rotation speed (encoder pulse count since last report; higher = faster turn)
//         NOT a monotonic counter — stays at 1 during slow continuous rotation
//   [2] = 0x00 (unused)
//   [3] = direction: 0x01 = CW, 0x02 = CCW; stays set while rotating, 0x00 when idle
//   [4] = 0x00 (unused)
//   [5] = button state bitmask (active-low): 0x07 = all idle,
//         bit0=TX/PTT, bit1=F1, bit2=F2

HidEvent IcomRC28Parser::parse(const uint8_t* buf, size_t len)
{
    if (len < 6) return {};
    if (buf[0] != 0x01) return {};

    const uint8_t speed = buf[1];
    const uint8_t dir   = buf[3];
    const uint8_t btns  = buf[5];

    // Button events (active-low bitmask → same enum values as before).
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

    // Rotation: one event per report, magnitude proportional to speed (buf[1]).
    // Guard: skip when any button is held (btns != 0x07) so accidental knob
    // movement during button presses doesn't retune — matches wfview behaviour.
    if (btns == 0x07 && speed > 0 && (dir == 0x01 || dir == 0x02)) {
        const int sign = (dir == 0x01) ? 1 : -1;
        return {HidEvent::Rotate, sign * static_cast<int>(speed), 0, 0};
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

// ── Elgato StreamDeck+ ─────────────────────────────────────────────────────
// 14-byte reports. hidapi always includes the report ID as buf[0] = 0x01.
// Protocol verified against python-elgato-streamdeck v0.9.8 source:
//   buf[0]  = 0x01 (report ID — strip it)
//   buf[1]  = event type: 0x00=key, 0x02=touchscreen, 0x03=dial
//   buf[2..3] = reserved
//   Dial event (buf[1]==0x03):
//     buf[4] = sub-type: 0x01=turn, 0x00=push
//     buf[5..8] = 4 encoder values (signed int8 delta for turn, bool for push)
//   Key event (buf[1]==0x00):
//     buf[4..11] = 8 LCD key states (0=up, 1=down)
// Button numbering: LCD keys 1-8, encoder press buttons 9-12.

HidEvent StreamDeckPlusParser::parse(const uint8_t* buf, size_t len)
{
    if (len < 9) return {};

    // buf[0] = report ID (0x01) — always present in hidraw reads on all platforms
    const uint8_t type    = buf[1];
    const uint8_t subtype = buf[4];

    if (type == 0x03) {
        if (subtype == 0x01) {
            // Encoder turn — return first non-zero delta
            for (int i = 0; i < 4; ++i) {
                int delta = static_cast<int>(static_cast<int8_t>(buf[5 + i]));
                if (delta != 0)
                    return {.type = HidEvent::Rotate, .steps = delta, .encoderIndex = i};
            }
        } else if (subtype == 0x00) {
            // Encoder push — return first changed encoder button.
            // We commit only the bit we're reporting (not all of newState) so
            // that if two buttons change in the same HID report, the next
            // poll() iteration's `changed` still flags the unreported bit and
            // emits it.  See #3248 follow-up from PR #3236 review.
            uint8_t newState = 0;
            for (int i = 0; i < 4; ++i) {
                if (buf[5 + i]) newState |= (1u << i);
            }
            uint8_t changed = newState ^ m_prevEncBtns;
            for (int i = 0; i < 4; ++i) {
                if (changed & (1u << i)) {
                    m_prevEncBtns ^= (1u << i);  // consume this bit only
                    const int act = (newState & (1u << i)) ? 0 : 1;
                    return {.type = HidEvent::Button, .button = 9 + i, .action = act};
                }
            }
        }
        return {};
    }

    if (type == 0x00) {
        // LCD key — return first changed key.  Same one-bit-at-a-time
        // commit pattern as encoder push above so simultaneous presses
        // are not silently dropped. (#3248)
        if (len < 12) return {};
        uint8_t newState = 0;
        for (int i = 0; i < 8; ++i) {
            if (buf[4 + i]) newState |= (1u << i);
        }
        uint8_t changed = newState ^ m_prevKeys;
        for (int i = 0; i < 8; ++i) {
            if (changed & (1u << i)) {
                m_prevKeys ^= (1u << i);  // consume this bit only
                const int act = (newState & (1u << i)) ? 0 : 1;
                return {.type = HidEvent::Button, .button = i + 1, .action = act};
            }
        }
        return {};
    }

    return {};
}

// ── ELAD/WoodBoxRadio TMate 2 ──────────────────────────────────────────────
// 64-byte reports. Keys are active-low (bit clear = pressed, bit set = idle).
// Encoder counters are absolute uint16 with natural 16-bit wrap.
// Delta wrap-correction: same algorithm as ShuttleXpress jog (±65536 adjust).

HidEvent TMate2Parser::parse(const uint8_t* buf, size_t len)
{
    if (len < 9) return {};

    const uint16_t enc[3] = {
        static_cast<uint16_t>(buf[1] | (static_cast<uint16_t>(buf[2]) << 8)),
        static_cast<uint16_t>(buf[3] | (static_cast<uint16_t>(buf[4]) << 8)),
        static_cast<uint16_t>(buf[5] | (static_cast<uint16_t>(buf[6]) << 8)),
    };
    const uint16_t keys = static_cast<uint16_t>(buf[7] | (static_cast<uint16_t>(buf[8]) << 8));

    if (m_firstReport) {
        m_firstReport = false;
        m_enc[0] = enc[0];
        m_enc[1] = enc[1];
        m_enc[2] = enc[2];
        m_keys   = keys;
        return {};
    }

    // Encoders — report first non-zero wrap-corrected delta.
    for (int i = 0; i < 3; ++i) {
        int diff = static_cast<int>(enc[i]) - static_cast<int>(m_enc[i]);
        if (diff > 32767)  diff -= 65536;
        if (diff < -32768) diff += 65536;
        if (diff != 0) {
            m_enc[i] = enc[i];
            if (i == 0) diff = -diff;
            return {.type = HidEvent::Rotate, .steps = diff, .encoderIndex = i};
        }
    }

    // Keys (active-low) — one bit at a time so simultaneous presses are not lost.
    // F1-F6 (bits 0-5) → buttons 1-6.
    // Encoder pushes (bits 6-8) → buttons 9-11 so MainWindow routes them through
    // HidEncoderPushAction{0-2}: bit6=main-encoder(enc1)→9, bit7=enc2→10, bit8=enc3→11.
    // Buttons 7-8 are intentionally unused (no hardware key on those numbers).
    static constexpr int kButtonMap[9] = {1, 2, 3, 4, 5, 6, 9, 10, 11};
    if (keys != m_keys) {
        const uint16_t changed = keys ^ m_keys;
        for (int b = 0; b < 9; ++b) {
            const uint16_t mask = static_cast<uint16_t>(1u << b);
            if (changed & mask) {
                m_keys ^= mask;
                // bit set = idle/released (action 1); bit clear = pressed (action 0)
                const int action = (keys & mask) ? 1 : 0;
                return {.type = HidEvent::Button, .button = kButtonMap[b], .action = action};
            }
        }
    }

    return {};
}

} // namespace AetherSDR
#endif
