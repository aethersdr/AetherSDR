#pragma once

#include "EspFlashTarget.h"

#include <QList>
#include <QString>

#include <memory>

namespace AetherSDR {

// An ESP32-family chip in ROM download mode on a serial port: Espressif's
// esp-serial-flasher (third_party/esp-serial-flasher) speaking through
// QSerialPort. Connecting uploads Espressif's flasher stub, which reads flash
// quickly and checks writes with MD5. Every call blocks, so use it from a
// worker thread, and from one thread only.
//
// Only devices with a built-in USB serial/JTAG port (ESP32-S3, -C3, -C6, ...)
// are found by listDevices(): they need no reset wiring, and the user puts
// them in download mode (BOOT held while RESET is tapped).
class EspFlasher : public EspFlashTarget {
public:
    struct Device {
        QString portName;       // for QSerialPort
        QString serialNumber;   // the USB serial number: the chip's MAC
    };
    // Serial ports of ESP chips in download mode over their USB serial/JTAG
    // port (Espressif 303A:1001, with the chip's MAC as serial number).
    // Firmware that enumerates with the same IDs is not listed.
    static QList<Device> listDevices();

    EspFlasher();
    ~EspFlasher() override;

    // Opens portName, connects to the ROM bootloader and starts the stub.
    // accessDenied (optional) is set when the OS refused to open the port.
    bool open(const QString& portName, QString* error, bool* accessDenied = nullptr);
    void close();
    bool isOpen() const;

    QString chipName() const override;
    QByteArray mac() const override;
    quint32 flashSize() const override;
    bool read(quint32 address, quint32 length, QByteArray* out,
              const Progress& progress, QString* error) override;
    bool write(quint32 address, const QByteArray& data,
               const Progress& progress, QString* error) override;
    bool matches(quint32 address, const QByteArray& data) override;
    bool restart(QString* error) override;

private:
    struct Session;
    std::unique_ptr<Session> m_session;
};

} // namespace AetherSDR
