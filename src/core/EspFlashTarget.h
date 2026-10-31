#pragma once

#include <QByteArray>
#include <QString>

#include <functional>

namespace AetherSDR {

// The flash of an ESP32-family chip held in its ROM download mode, as the
// firmware installer sees it. EspFlasher implements it over a serial port;
// tests implement it over a byte array.
class EspFlashTarget {
public:
    // Called as work proceeds; returning false cancels the operation.
    using Progress = std::function<bool(qint64 done, qint64 total)>;

    static constexpr quint32 kSectorSize = 0x1000;   // the unit of erasure

    virtual ~EspFlashTarget() = default;

    virtual QString chipName() const = 0;      // e.g. "ESP32-S3"
    virtual QByteArray mac() const = 0;        // 6 bytes, the chip's base MAC
    virtual quint32 flashSize() const = 0;     // bytes

    // Reads length bytes at address into out.
    virtual bool read(quint32 address, quint32 length, QByteArray* out,
                      const Progress& progress, QString* error) = 0;
    // Writes data at address, which must be sector-aligned; the sectors it
    // covers are erased first, so the rest of its last sector becomes 0xFF.
    // Verifies what was written before returning true.
    virtual bool write(quint32 address, const QByteArray& data,
                       const Progress& progress, QString* error) = 0;
    // Whether the length bytes at address already hold exactly data's MD5.
    // False when the target cannot tell, so callers just write.
    virtual bool matches(quint32 address, const QByteArray& data) = 0;
    // Leaves download mode and starts the firmware the device's boot
    // selection names.
    virtual bool restart(QString* error) = 0;
};

} // namespace AetherSDR
