#pragma once
#ifdef HAVE_HIDAPI

#include <QObject>
#include <QTimer>
#include <QByteArray>
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
    bool isOpen() const { return m_device != nullptr; }
    QString deviceName() const { return m_deviceName; }
    uint16_t vendorId() const { return m_openVid; }
    uint16_t productId() const { return m_openPid; }
    int encoderCount() const { return m_parser ? m_parser->encoderCount() : 1; }
    bool isStreamDeckPlus() const { return m_openVid == 0x0FD9 && m_openPid == 0x0084; }

    void setInvertDirection(bool invert) { m_invertDirection = invert; }

public slots:
    void loadSettings();
    // Write 120x120 JPEG images to StreamDeck+ LCD keys. Pass all 8 images at once
    // so one queued call updates the whole display without flooding the event queue.
    // No-op if device is not a StreamDeck+.
    void setKeyImages(const QVector<QByteArray>& jpegImages);
    void setKeyImage(int key, const QByteArray& jpegData);

signals:
    void tuneSteps(int encoderIndex, int steps);
    void buttonPressed(int button, int action);
    void connectionChanged(bool connected, const QString& deviceName);

private slots:
    void poll();
    void hotplugCheck();

private:
    hid_device* m_device{nullptr};
    std::unique_ptr<HidDeviceParser> m_parser;
    QString m_deviceName;
    uint16_t m_openVid{0};
    uint16_t m_openPid{0};
    bool m_invertDirection{false};

    QTimer* m_pollTimer{nullptr};
    QTimer* m_hotplugTimer{nullptr};
    uint8_t m_buf[64]{};

    static constexpr int POLL_INTERVAL_MS = 5;
    static constexpr int HOTPLUG_INTERVAL_MS = 3000;
};

} // namespace AetherSDR
#endif
