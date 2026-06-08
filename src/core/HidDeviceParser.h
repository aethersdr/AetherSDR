#pragma once
#ifdef HAVE_HIDAPI

#include <cstdint>
#include <cstddef>
#include <memory>

namespace AetherSDR {

struct HidEvent {
    enum Type { None, Rotate, Button };
    Type type{None};
    int  steps{0};           // for Rotate: +CW, -CCW
    int  button{0};          // 1-based button number
    int  action{0};          // 0=press, 1=release
    int  encoderIndex{0};    // for multi-encoder devices: 0-based dial index (last so existing {type,steps,button,action} inits stay valid)
};

struct HidDeviceId {
    uint16_t vid;
    uint16_t pid;
    const char* name;
};

class HidDeviceParser {
public:
    virtual ~HidDeviceParser() = default;
    virtual HidEvent parse(const uint8_t* buf, size_t len) = 0;
    virtual size_t reportSize() const = 0;
    virtual int encoderCount() const { return 1; }

    static std::unique_ptr<HidDeviceParser> create(uint16_t vid, uint16_t pid);
    static const HidDeviceId* supportedDevices();
    static int supportedDeviceCount();
};

// Icom RC-28 (VID 0x0C26, PID 0x001E)
// 32-byte reports, no report ID prefix (hidraw returns exactly 32 bytes).
// Actual layout (verified from hardware, FlexRC-28 driver, and wfview):
//   [0]=0x01 constant, [1]=rotation speed (pulse count, NOT a monotonic counter),
//   [2]=0x00, [3]=direction (0x01=CW, 0x02=CCW, stays set while rotating),
//   [4]=0x00, [5]=button state (active-low bitmask, 0x07=all idle).
class IcomRC28Parser : public HidDeviceParser {
public:
    HidEvent parse(const uint8_t* buf, size_t len) override;
    size_t reportSize() const override { return 32; }
private:
    uint8_t m_prevButtonState{0x07};  // 0x07 = all released (idle)
};

// Griffin PowerMate (VID 0x077D, PID 0x0410)
class GriffinPowerMateParser : public HidDeviceParser {
public:
    HidEvent parse(const uint8_t* buf, size_t len) override;
    size_t reportSize() const override { return 6; }
private:
    uint8_t m_prevButton{0};
};

// Contour ShuttleXpress (VID 0x0B33, PID 0x0020)
class ShuttleXpressParser : public HidDeviceParser {
public:
    HidEvent parse(const uint8_t* buf, size_t len) override;
    size_t reportSize() const override { return 5; }
private:
    uint8_t m_prevJog{0};
    uint8_t m_prevButtons{0};
    bool m_firstReport{true};
};

// Contour ShuttlePro v2 (VID 0x0B33, PID 0x0030)
class ShuttleProV2Parser : public HidDeviceParser {
public:
    HidEvent parse(const uint8_t* buf, size_t len) override;
    size_t reportSize() const override { return 5; }
private:
    uint8_t m_prevJog{0};
    uint16_t m_prevButtons{0};
    bool m_firstReport{true};
};

// Elgato StreamDeck+ (VID 0x0FD9, PID 0x0084)
// 14-byte reports (device HID descriptor advertises 512 but only first 14
// bytes carry event data; matching python-elgato-streamdeck library behaviour).
// hidapi always includes the 1-byte report ID (0x01) as buf[0] on all platforms.
// Layout (all indices into raw buf[]):
//   [0] report ID = 0x01 (always present — strip it)
//   [1] event type: 0x00=key state, 0x02=touchscreen, 0x03=dial (encoder)
//   [2..3] reserved/padding
//   Dial event ([1]==0x03):
//     [4] sub-type: 0x01=turn, 0x00=push
//     [5..8] 4 encoder values (signed int8 delta for turn, bool for push)
//   Key event ([1]==0x00):
//     [4..11] 8 LCD key states (0=up, 1=down)
// Button numbering: LCD keys 1-8, encoder press buttons 9-12.
class StreamDeckPlusParser : public HidDeviceParser {
public:
    HidEvent parse(const uint8_t* buf, size_t len) override;
    size_t reportSize() const override { return 14; }
    int encoderCount() const override { return 4; }
private:
    uint8_t m_prevKeys{0};         // bitmask of previous LCD key states (bits 0-7)
    uint8_t m_prevEncBtns{0};      // bitmask of previous encoder button states (bits 0-3)
};

// ELAD/WoodBoxRadio TMate 2 (VID 0x1721, PID 0x0614)
// 64-byte HID reports (full USB interrupt report size).
// Input layout (bytes 0-8 mapped; 9-63 not yet fully decoded):
//   [0]    report ID = 0x01
//   [1..2] encoder 1, little-endian uint16 (absolute wrapping counter)
//   [3..4] encoder 2, little-endian uint16
//   [5..6] encoder 3 (volume), little-endian uint16
//   [7..8] key bitmask, little-endian uint16, active-low (bit clear = pressed):
//          bit0=F1, bit1=F2, bit2=F3, bit3=F4, bit4=F5, bit5=F6,
//          bit6=enc1/main-encoder push, bit7=enc2 push, bit8=enc3 push.
//          Idle state: 0x01FF (all bits set).
// Encoder delta uses 16-bit wrap correction (same as ShuttleXpress jog).
// encoderIndex: 0=enc1/main-tuning (bytes 1-2), 1=enc2/TX-power (bytes 3-4), 2=enc3/volume (bytes 5-6).
// Buttons: 1-6=F1-F6, 9=main-encoder push (enc1), 10=encoder2 push (enc2), 11=encoder3 push (enc3).
// Encoder push buttons use the 9+ range to route through HidEncoderPushAction{0-2} in MainWindow.
// Note: bit6=$0040=main-encoder push, bit7=$0080=enc2 push, bit8=$0100=enc3 push (hardware naming quirk).
// Protocol reverse-engineered via USBPcap; documented in OpenTMate2Lib.
class TMate2Parser : public HidDeviceParser {
public:
    HidEvent parse(const uint8_t* buf, size_t len) override;
    size_t reportSize() const override { return 64; }
    int encoderCount() const override { return 3; }
private:
    uint16_t m_enc[3]{};
    uint16_t m_keys{0x01FFu};   // idle: all bits set
    bool m_firstReport{true};
};

} // namespace AetherSDR
#endif
