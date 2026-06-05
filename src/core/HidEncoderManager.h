#pragma once
#ifdef HAVE_HIDAPI

#include <QObject>
#include <QTimer>
#include <QByteArray>
#include <atomic>
#include <memory>
#include <hidapi/hidapi.h>
#include "HidDeviceParser.h"

namespace AetherSDR {

// Manages a USB HID encoder device (Icom RC-28, Griffin PowerMate,
// Contour ShuttleXpress/Pro). Runs on the ExtControllers thread.
// Polls the device via hidapi and emits tuneSteps/buttonPressed
// signals, same pattern as FlexControlManager. (#616)
class HidEncoderManager : public QObject {
    Q_OBJECT

public:
    explicit HidEncoderManager(QObject* parent = nullptr);
    ~HidEncoderManager() override;

    // Scan for any supported HID device
    static QString detectDevice();

    bool open(uint16_t vid, uint16_t pid);
    void close();
    // These getters are called from the main thread (e.g. refreshStreamDeckLabels,
    // status snapshot) while the worker thread on m_extCtrlThread mutates the
    // backing members in open()/close()/hotplugCheck().  Underlying state is
    // std::atomic so the cross-thread reads are well-defined; relaxed ordering
    // is sufficient because callers treat the result as a hint and the real
    // gate is re-checked inside the queued slot. (#3248)
    bool isOpen() const { return m_device.load(std::memory_order_relaxed) != nullptr; }
    bool isBlockedByMultiple() const { return m_multipleDetected.load(std::memory_order_acquire); }
    QString blockedDeviceName()  const { return m_blockedDeviceName; }
    QString deviceName()     const { return m_deviceName; }
    QString devicePath()     const { return m_devicePath; }
    QString serialNumber()   const { return m_serialNumber; }
    uint16_t releaseNumber() const { return m_releaseNumber; }
    uint16_t vendorId() const { return m_openVid.load(std::memory_order_relaxed); }
    uint16_t productId() const { return m_openPid.load(std::memory_order_relaxed); }
    int encoderCount() const { return m_parser ? m_parser->encoderCount() : 1; }
    bool isStreamDeckPlus() const {
        return m_openVid.load(std::memory_order_relaxed) == 0x0FD9
            && m_openPid.load(std::memory_order_relaxed) == 0x0084;
    }
    bool isTMate2() const {
        return m_openVid.load(std::memory_order_relaxed) == 0x1721
            && m_openPid.load(std::memory_order_relaxed) == 0x0614;
    }
    // Single source of truth for the RC-28-compatible VID/PID set, shared by the
    // instance check below and open()'s multi-device guard. The emulator runs
    // the same wire protocol as the real RC-28, including the LED output report.
    static bool isRC28CompatibleId(uint16_t vid, uint16_t pid) {
        return (vid == 0x0C26 && pid == 0x001E)   // Icom RC-28
            || (vid == 0x2341 && pid == 0x0266);   // AetherPad emulator
    }
    bool isRC28Compatible() const {
        return isRC28CompatibleId(m_openVid.load(std::memory_order_relaxed),
                                  m_openPid.load(std::memory_order_relaxed));
    }

    void setInvertDirection(bool invert) { m_invertDirection = invert; }

    // RC-28 button mapping persists as a single nested-JSON blob under the
    // "RC28Mapping" AppSettings key (Principle V): one root key per feature,
    // not a stack of flat keys. Fields: f1Press, f1Hold, f2Press, f2Hold,
    // pttMode. These helpers are the single read/write path shared by the
    // mapping dialog and the MainWindow dispatch/LED code. (#3323)
    static QString rc28MappingField(const QString& field, const QString& dflt);
    static void setRc28MappingField(const QString& field, const QString& value);

    // Active-low LED byte constant for setRC28Leds(). updateRC28Leds() in
    // MainWindow builds the full byte bitwise; RC28_LEDS_OFF is the reset value.
    static constexpr uint8_t RC28_LEDS_OFF = 0x0F;  // all LEDs off

public slots:
    void loadSettings();
    // Set the LED state on an RC-28-compatible device. ledByte is active-low:
    //   bit0=TX/PTT, bit1=F1, bit2=F2, bit3=LINK. Clear bit = LED on.
    // No-op if the connected device is not RC-28-compatible.
    void setRC28Leds(uint8_t ledByte);
    // Write 120x120 JPEG images to StreamDeck+ LCD keys. Pass all 8 images at once
    // so one queued call updates the whole display without flooding the event queue.
    // No-op if device is not a StreamDeck+.
    void setKeyImages(const QVector<QByteArray>& jpegImages);
    void setKeyImage(int key, const QByteArray& jpegData);
    // Set the RGB backlight on a TMate 2. No-op if device is not a TMate 2.
    void setTMate2Backlight(uint8_t r, uint8_t g, uint8_t b);
    // Set the LED status byte on a TMate 2 (byte 32 of the LCDVector).
    //   bit0 = USB/radio connected, bit1 = VFO locked.
    // No-op if device is not a TMate 2.
    void setTMate2Status(uint8_t led_byte);
    // Write frequency and S-meter/power value to the TMate 2 LCD.
    //   freq_hz   : displayed right-aligned across the 9-digit main display.
    //   small_val : displayed on the 3-digit S-meter/power display (mod 1000).
    // Sends a full LCDVector update (backlight, contrast, timing are preserved).
    // No-op if device is not a TMate 2.
    void setTMate2Display(uint32_t freq_hz, uint32_t small_val);
    // Update the TMate 2 segment indicators (RX/TX, mode, S-meter bargraph,
    // RIT/XIT, decimal dots).  Call whenever any of these state items changes.
    //   tx        : true = transmitting, false = receiving
    //   mode      : demodulation mode string ("USB","LSB","AM","FM","CW","DIGL","DIGU",…)
    //   smeter_dbm: S-meter reading in dBm; drives the 15-segment bargraph
    //   rit/xit   : RIT / XIT active flags
    // No-op if device is not a TMate 2.
    void setTMate2Indicators(bool tx, const QString& mode, float smeter_dbm,
                              bool rit, bool xit);
    // Temporarily switch indicator segments to a TMate 2 overlay view.
    // overlayType: "volume", "power", "speed", "wpm", or "rit".
    void setTMate2OverlayIndicators(const QString& overlayType,
                                     int overlayValue,
                                     const QString& mode);
    // Clear all non-digit TMate 2 indicator segments. Used by the idle blanker.
    void clearTMate2Indicators();
    // Write an 800x100 JPEG to the touchscreen strip above the dials.
    // x_pos/y_pos/width/height let you update a sub-region; defaults write the full strip.
    void setTouchscreenImage(const QByteArray& jpegData,
                             int x_pos = 0, int y_pos = 0,
                             int width = 800, int height = 100);

signals:
    void tuneSteps(int encoderIndex, int steps);
    void buttonPressed(int button, int action);
    void connectionChanged(bool connected, const QString& deviceName);
    // Emitted when open() finds more than one device with the same VID/PID.
    // The device is not opened; hotplug will retry until only one remains.
    void multipleDevicesDetected(const QString& deviceName);

private slots:
    void poll();
    void hotplugCheck();

private:
    // m_device + m_openVid + m_openPid are read from the main thread
    // (isOpen / isStreamDeckPlus / vendorId / productId) and written from
    // m_extCtrlThread (open / close / hotplugCheck).  std::atomic makes
    // those cross-thread reads well-defined.  m_deviceName / m_devicePath /
    // m_serialNumber / m_releaseNumber are also touched cross-thread but are
    // const-after-open and only used for diagnostics — brief stale reads are
    // benign, matching the existing m_deviceName convention. (#3248, #3323)
    std::atomic<hid_device*> m_device{nullptr};
    std::unique_ptr<HidDeviceParser> m_parser;
    QString m_deviceName;
    QString m_devicePath;
    QString m_serialNumber;
    uint16_t m_releaseNumber{0};
    // Latches true while open() is blocking on >1 RC-28 so the warning + signal
    // fire once per transition, not on every hotplug retry. Atomic so the main
    // thread can poll isBlockedByMultiple() without a data race. The companion
    // m_blockedDeviceName QString is written before this flag is set (release
    // store) and read after it is tested (acquire load), establishing the
    // happens-before needed to read the QString safely. (#3323)
    std::atomic<bool> m_multipleDetected{false};
    QString m_blockedDeviceName;
    std::atomic<uint16_t> m_openVid{0};
    std::atomic<uint16_t> m_openPid{0};
    bool m_invertDirection{false};

    // Persistent LCDVector state for TMate 2 (44 bytes).  All output functions
    // modify this buffer then send the full 64-byte report so that backlight,
    // contrast, and timing fields are never accidentally reset to zero.
    // Initialised to zeros; timing defaults are applied in open() when a
    // TMate 2 is detected.  Protocol documented in OpenTMate2Lib.
    uint8_t m_lcdVector[44]{};

    // Send the current m_lcdVector to the device.  Pads to 64 bytes with zeros.
    // Must only be called while m_device is open and isTMate2() is true.
    void sendTMate2();

    QTimer* m_pollTimer{nullptr};
    QTimer* m_hotplugTimer{nullptr};
    uint8_t m_buf[64]{};

    static constexpr int POLL_INTERVAL_MS = 5;
    static constexpr int HOTPLUG_INTERVAL_MS = 3000;
};

} // namespace AetherSDR
#endif
