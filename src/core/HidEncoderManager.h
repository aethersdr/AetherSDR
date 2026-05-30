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
    QString deviceName() const { return m_deviceName; }
    uint16_t vendorId() const { return m_openVid.load(std::memory_order_relaxed); }
    uint16_t productId() const { return m_openPid.load(std::memory_order_relaxed); }
    int encoderCount() const { return m_parser ? m_parser->encoderCount() : 1; }
    bool isStreamDeckPlus() const {
        return m_openVid.load(std::memory_order_relaxed) == 0x0FD9
            && m_openPid.load(std::memory_order_relaxed) == 0x0084;
    }
    // True for the real Icom RC-28 and the AetherPad emulator — both run the
    // same wire protocol including the LED output report.
    bool isRC28Compatible() const {
        const uint16_t vid = m_openVid.load(std::memory_order_relaxed);
        const uint16_t pid = m_openPid.load(std::memory_order_relaxed);
        return (vid == 0x0C26 && pid == 0x001E)   // Icom RC-28
            || (vid == 0x2341 && pid == 0x0266);   // AetherPad emulator
    }

    void setInvertDirection(bool invert) { m_invertDirection = invert; }

    // Active-low LED byte constants for setRC28Leds().
    static constexpr uint8_t RC28_LEDS_OFF      = 0x0F;  // all LEDs off
    static constexpr uint8_t RC28_LEDS_LINK     = 0x07;  // LINK on, rest off
    static constexpr uint8_t RC28_LEDS_TX_LINK  = 0x06;  // TX + LINK on

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
    // Write an 800x100 JPEG to the touchscreen strip above the dials.
    // x_pos/y_pos/width/height let you update a sub-region; defaults write the full strip.
    void setTouchscreenImage(const QByteArray& jpegData,
                             int x_pos = 0, int y_pos = 0,
                             int width = 800, int height = 100);

signals:
    void tuneSteps(int encoderIndex, int steps);
    void buttonPressed(int button, int action);
    void connectionChanged(bool connected, const QString& deviceName);

private slots:
    void poll();
    void hotplugCheck();

private:
    // m_device + m_openVid + m_openPid are read from the main thread
    // (isOpen / isStreamDeckPlus / vendorId / productId) and written from
    // m_extCtrlThread (open / close / hotplugCheck).  std::atomic makes
    // those cross-thread reads well-defined.  m_deviceName is also
    // touched cross-thread but it's a const-after-open string used only
    // in diagnostics so a brief stale read is benign. (#3248)
    std::atomic<hid_device*> m_device{nullptr};
    std::unique_ptr<HidDeviceParser> m_parser;
    QString m_deviceName;
    std::atomic<uint16_t> m_openVid{0};
    std::atomic<uint16_t> m_openPid{0};
    bool m_invertDirection{false};

    QTimer* m_pollTimer{nullptr};
    QTimer* m_hotplugTimer{nullptr};
    uint8_t m_buf[64]{};

    static constexpr int POLL_INTERVAL_MS = 5;
    static constexpr int HOTPLUG_INTERVAL_MS = 3000;
};

} // namespace AetherSDR
#endif
