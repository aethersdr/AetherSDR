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
    // Further events from the report last given to parse(); the caller drains
    // this until None, so one report can carry several edges. Default: none.
    virtual HidEvent nextPending() { return {}; }
    // KEEP THIS <= 64: it bounds hid_read() into HidEncoderManager::m_buf, a fixed
    // uint8_t[64], so a larger value overflows it. TMate2 already returns 64. A device
    // needing more must grow m_buf (or clamp at hid_read) in the same change.
    // Returning less than the real report size is fine (StreamDeck+ returns 14).
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

// Shared decoder for the Contour 5-byte report (ShuttleXpress, ShuttlePro v2).
// Every button edge in a report, then the jog delta, is queued: parse()
// returns the first and nextPending() the rest, so simultaneous changes are
// never dropped.
class ContourShuttleParser : public HidDeviceParser {
public:
    HidEvent parse(const uint8_t* buf, size_t len) override;
    HidEvent nextPending() override;
    size_t reportSize() const override { return 5; }
protected:
    // Buttons packed into bits 0..buttonCount()-1 (1-based button = bit + 1).
    virtual uint16_t buttonMask(const uint8_t* buf) const = 0;
    virtual int buttonCount() const = 0;
private:
    static constexpr int kMaxEvents = 16 + 1;  // up to 16 button bits + jog
    HidEvent m_pending[kMaxEvents];
    int m_pendingHead{0};
    int m_pendingCount{0};
    uint16_t m_prevButtons{0};
    uint8_t m_prevJog{0};
    bool m_firstReport{true};
};

// Contour ShuttleXpress (VID 0x0B33, PID 0x0020)
class ShuttleXpressParser : public ContourShuttleParser {
protected:
    uint16_t buttonMask(const uint8_t* buf) const override;
    int buttonCount() const override { return 5; }
};

// Contour ShuttlePro v2 (VID 0x0B33, PID 0x0030)
class ShuttleProV2Parser : public ContourShuttleParser {
protected:
    uint16_t buttonMask(const uint8_t* buf) const override;
    int buttonCount() const override { return 15; }
};

// Elgato StreamDeck+ (VID 0x0FD9, PID 0x0084). 14-byte reports (the descriptor
// says 512; only 14 carry data). hidapi includes report ID 0x01 as buf[0].
//   [0] report ID 0x01 (strip)
//   [1] event: 0x00 key, 0x02 touchscreen, 0x03 dial
//   Dial ([1]==0x03): [4] 0x01 turn / 0x00 push; [5..8] 4 encoders (int8 delta
//                     for turn, bool for push)
//   Key  ([1]==0x00): [4..11] 8 LCD key states (0 up, 1 down)
// Buttons: LCD keys 1-8, encoder presses 9-12.
class StreamDeckPlusParser : public HidDeviceParser {
public:
    HidEvent parse(const uint8_t* buf, size_t len) override;
    size_t reportSize() const override { return 14; }
    int encoderCount() const override { return 4; }
private:
    uint8_t m_prevKeys{0};         // bitmask of previous LCD key states (bits 0-7)
    uint8_t m_prevEncBtns{0};      // bitmask of previous encoder button states (bits 0-3)
};

// ELAD/WoodBoxRadio TMate 2 (VID 0x1721, PID 0x0614), 64-byte reports (bytes
// 9-63 not decoded); reverse-engineered via USBPcap (OpenTMate2Lib):
//   [0]    report ID 0x01
//   [1..2] enc1 (main tuning), [3..4] enc2 (TX power), [5..6] enc3 (volume):
//          LE uint16 wrapping counters, 16-bit wrap correction
//   [7..8] keys, LE uint16, active-low (idle 0x01FF): bit0-5 F1-F6,
//          bit6 enc1 push, bit7 enc2 push, bit8 enc3 push
// Buttons: 1-6 = F1-F6, 9/10/11 = enc1/2/3 push (routes to
// HidEncoderPushAction{0-2} in MainWindow).
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
