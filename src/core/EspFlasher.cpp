#include "EspFlasher.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QThread>

#include <cstddef>

extern "C" {
#include "esp_loader.h"
#include "esp_loader_io.h"
}

namespace AetherSDR {

namespace {

constexpr quint16 kEspressifVid = 0x303A;
constexpr quint16 kUsbSerialJtagPid = 0x1001;

constexpr quint32 kStubWriteBlock = 0x4000;
constexpr quint32 kReadChunk = 0x10000;   // per call, so progress can be reported

// ESP32-S3: leave download mode through the RTC watchdog. A USB serial/JTAG
// device has no reset line to pulse, and toggling RTS resets the chip back
// into download mode. Constants as esptool's targets/esp32s3.py.
constexpr quint32 kS3RtcCntlOption1 = 0x6000812C;
constexpr quint32 kS3ForceDownloadBoot = 0x1;
constexpr quint32 kS3WdtConfig0 = 0x60008098;
constexpr quint32 kS3WdtConfig1 = 0x6000809C;
constexpr quint32 kS3WdtProtect = 0x600080B0;
constexpr quint32 kS3WdtKey = 0x50D83AA1;

QString chipLabel(target_chip_t chip)
{
    switch (chip) {
    case ESP8266_CHIP: return QStringLiteral("ESP8266");
    case ESP32_CHIP: return QStringLiteral("ESP32");
    case ESP32S2_CHIP: return QStringLiteral("ESP32-S2");
    case ESP32C3_CHIP: return QStringLiteral("ESP32-C3");
    case ESP32S3_CHIP: return QStringLiteral("ESP32-S3");
    case ESP32C2_CHIP: return QStringLiteral("ESP32-C2");
    case ESP32C5_CHIP: return QStringLiteral("ESP32-C5");
    case ESP32H2_CHIP: return QStringLiteral("ESP32-H2");
    case ESP32C6_CHIP: return QStringLiteral("ESP32-C6");
    case ESP32P4_CHIP: return QStringLiteral("ESP32-P4");
    case ESP32C61_CHIP: return QStringLiteral("ESP32-C61");
    default: return QStringLiteral("unknown");
    }
}

QString describe(esp_loader_error_t err)
{
    switch (err) {
    case ESP_LOADER_SUCCESS: return {};
    case ESP_LOADER_ERROR_TIMEOUT:
        return QCoreApplication::translate("EspFlasher", "the device stopped answering");
    case ESP_LOADER_ERROR_IMAGE_SIZE:
        return QCoreApplication::translate("EspFlasher", "beyond the end of the flash");
    case ESP_LOADER_ERROR_INVALID_MD5:
        return QCoreApplication::translate("EspFlasher", "what was written does not read back the same");
    case ESP_LOADER_ERROR_INVALID_PARAM:
        return QCoreApplication::translate("EspFlasher", "an address or size the chip cannot use");
    case ESP_LOADER_ERROR_INVALID_TARGET:
    case ESP_LOADER_ERROR_UNSUPPORTED_CHIP:
        return QCoreApplication::translate("EspFlasher", "an unsupported chip");
    case ESP_LOADER_ERROR_UNSUPPORTED_FUNC:
        return QCoreApplication::translate("EspFlasher", "not supported by this chip");
    default:
        return QCoreApplication::translate("EspFlasher", "a protocol error");
    }
}

// esp_loader_port_t for a QSerialPort; the library finds it from its
// esp_loader_port_t member with container_of.
struct QtPort {
    esp_loader_port_t port;
    QSerialPort* serial{nullptr};
    QElapsedTimer timer;
    quint32 timerMs{0};
};

QtPort* qtPort(esp_loader_port_t* port)
{
    return container_of(port, QtPort, port);
}

// Restarts the chip into a fresh ROM bootloader: the USB serial/JTAG port
// turns DTR/RTS changes into its reset and boot-strap lines, so a stub left
// running by an earlier session does not stand in the way of a new one. The
// port stays on the bus. As esp_pylib's usb_jtag_bootloader_reset; RTS is
// re-written after DTR changes because Windows' usbser.sys sends the line
// state only on an RTS change.
void usbJtagBootloaderReset(esp_loader_port_t* port)
{
    QSerialPort* serial = qtPort(port)->serial;
    const auto lines = [serial](bool dtr, bool rts) {
        serial->setDataTerminalReady(dtr);
        serial->setRequestToSend(rts);
    };
    lines(false, false);   // idle
    QThread::msleep(100);
    lines(true, false);    // BOOT low
    QThread::msleep(100);
    serial->setRequestToSend(true);   // reset, through (1,1) rather than (0,0)
    lines(false, true);
    QThread::msleep(100);
    lines(false, false);   // out of reset, into download mode
    serial->clear();
}

// Nothing to do: restart() leaves download mode through the watchdog.
void noReset(esp_loader_port_t*) {}

void startTimer(esp_loader_port_t* port, uint32_t ms)
{
    QtPort* p = qtPort(port);
    p->timerMs = ms;
    p->timer.start();
}

uint32_t remainingTime(esp_loader_port_t* port)
{
    QtPort* p = qtPort(port);
    const qint64 left = static_cast<qint64>(p->timerMs) - p->timer.elapsed();
    return left > 0 ? static_cast<uint32_t>(left) : 0;
}

void delayMs(esp_loader_port_t*, uint32_t ms)
{
    QThread::msleep(ms);
}

esp_loader_error_t changeRate(esp_loader_port_t* port, uint32_t rate)
{
    // A USB serial/JTAG port ignores the rate; set it anyway for UART bridges.
    return qtPort(port)->serial->setBaudRate(static_cast<qint32>(rate)) ? ESP_LOADER_SUCCESS
                                                                        : ESP_LOADER_ERROR_FAIL;
}

esp_loader_error_t portWrite(esp_loader_port_t* port, const uint8_t* data, uint16_t size,
                             uint32_t timeout)
{
    QSerialPort* serial = qtPort(port)->serial;
    QDeadlineTimer deadline(timeout);
    qint64 sent = 0;
    while (sent < size) {
        const qint64 n = serial->write(reinterpret_cast<const char*>(data) + sent, size - sent);
        if (n < 0)
            return ESP_LOADER_ERROR_FAIL;
        if (n == 0 && deadline.hasExpired())
            return ESP_LOADER_ERROR_TIMEOUT;
        sent += n;
    }
    while (serial->bytesToWrite() > 0) {
        if (!serial->waitForBytesWritten(static_cast<int>(deadline.remainingTime())))
            return ESP_LOADER_ERROR_TIMEOUT;
    }
    return ESP_LOADER_SUCCESS;
}

esp_loader_error_t portRead(esp_loader_port_t* port, uint8_t* data, uint16_t size,
                            uint32_t timeout)
{
    QSerialPort* serial = qtPort(port)->serial;
    QDeadlineTimer deadline(timeout);
    qint64 got = 0;
    while (got < size) {
        if (serial->bytesAvailable() == 0
            && !serial->waitForReadyRead(static_cast<int>(deadline.remainingTime())))
            return ESP_LOADER_ERROR_TIMEOUT;
        const qint64 n = serial->read(reinterpret_cast<char*>(data) + got, size - got);
        if (n < 0)
            return ESP_LOADER_ERROR_FAIL;
        got += n;
    }
    return ESP_LOADER_SUCCESS;
}

const esp_loader_port_ops_t kQtPortOps = [] {
    esp_loader_port_ops_t ops{};
    ops.enter_bootloader = usbJtagBootloaderReset;
    ops.reset_target = noReset;
    ops.start_timer = startTimer;
    ops.remaining_time = remainingTime;
    ops.delay_ms = delayMs;
    ops.change_transmission_rate = changeRate;
    ops.write = portWrite;
    ops.read = portRead;
    return ops;
}();

} // namespace

struct EspFlasher::Session {
    QSerialPort serial;
    QtPort port;
    esp_loader_t loader{};
    target_chip_t chip{ESP_UNKNOWN_CHIP};
    QByteArray mac;
    quint32 flashSize{0};
};

QList<EspFlasher::Device> EspFlasher::listDevices()
{
    QList<Device> devices;
    for (const QSerialPortInfo& info : QSerialPortInfo::availablePorts()) {
        // Firmware may enumerate with the same IDs through TinyUSB; only the
        // chip's own USB serial/JTAG port reports its MAC as serial number.
        static const QRegularExpression kMac(QStringLiteral("^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$"));
        if (info.hasVendorIdentifier() && info.vendorIdentifier() == kEspressifVid
            && info.hasProductIdentifier() && info.productIdentifier() == kUsbSerialJtagPid
            && kMac.match(info.serialNumber()).hasMatch())
            devices.append({info.portName(), info.serialNumber()});
    }
    return devices;
}

EspFlasher::EspFlasher() = default;

EspFlasher::~EspFlasher()
{
    close();
}

bool EspFlasher::open(const QString& portName, QString* error, bool* accessDenied)
{
    close();
    if (accessDenied)
        *accessDenied = false;
    auto s = std::make_unique<Session>();
    s->serial.setPortName(portName);
    s->serial.setBaudRate(115200);
    if (!s->serial.open(QIODevice::ReadWrite)) {
        if (s->serial.error() == QSerialPort::PermissionError) {
            if (accessDenied)
                *accessDenied = true;
            *error = QCoreApplication::translate(
                "EspFlasher", "Permission to open %1 was denied.").arg(portName);
        } else {
            *error = QCoreApplication::translate("EspFlasher", "Could not open %1: %2")
                         .arg(portName, s->serial.errorString());
        }
        return false;
    }
    s->port.port.ops = &kQtPortOps;
    s->port.serial = &s->serial;

    esp_loader_error_t err = esp_loader_init_serial(&s->loader, &s->port.port);
    esp_loader_connect_args_t args = ESP_LOADER_CONNECT_DEFAULT();
    if (err == ESP_LOADER_SUCCESS)
        err = esp_loader_connect_with_stub(&s->loader, &args);
    if (err != ESP_LOADER_SUCCESS) {
        *error = QCoreApplication::translate(
                     "EspFlasher", "No answer from the chip's bootloader on %1 (%2). Is it in "
                                   "download mode?")
                     .arg(portName, describe(err));
        return false;
    }
    s->chip = esp_loader_get_target(&s->loader);
    // The MAC is the device's identity: backups are matched to it, and an
    // action is checked against the device the operator confirmed.
    uint8_t mac[6] = {};
    err = esp_loader_read_mac(&s->loader, mac);
    if (err != ESP_LOADER_SUCCESS) {
        *error = QCoreApplication::translate("EspFlasher",
                                             "Could not read the chip's MAC address (%1).")
                     .arg(describe(err));
        return false;
    }
    s->mac = QByteArray(reinterpret_cast<const char*>(mac), sizeof mac);
    uint32_t size = 0;
    err = esp_loader_flash_detect_size(&s->loader, &size);
    if (err != ESP_LOADER_SUCCESS || size == 0) {
        *error = QCoreApplication::translate("EspFlasher", "Could not detect the flash size (%1).")
                     .arg(describe(err));
        return false;
    }
    s->flashSize = size;
    m_session = std::move(s);
    return true;
}

void EspFlasher::close()
{
    if (m_session) {
        esp_loader_deinit(&m_session->loader);
        m_session->serial.close();
        m_session.reset();
    }
}

bool EspFlasher::isOpen() const
{
    return m_session != nullptr;
}

QString EspFlasher::chipName() const
{
    return m_session ? chipLabel(m_session->chip) : QString();
}

QByteArray EspFlasher::mac() const
{
    return m_session ? m_session->mac : QByteArray();
}

quint32 EspFlasher::flashSize() const
{
    return m_session ? m_session->flashSize : 0;
}

bool EspFlasher::read(quint32 address, quint32 length, QByteArray* out, const Progress& progress,
                      QString* error)
{
    if (!m_session)
        return false;
    out->resize(static_cast<qsizetype>(length));
    for (quint32 done = 0; done < length;) {
        const quint32 n = qMin(kReadChunk, length - done);
        const esp_loader_error_t err = esp_loader_flash_read(
            &m_session->loader, reinterpret_cast<uint8_t*>(out->data()) + done, address + done, n);
        if (err != ESP_LOADER_SUCCESS) {
            *error = QCoreApplication::translate("EspFlasher", "Reading flash at 0x%1 failed: %2.")
                         .arg(address + done, 0, 16)
                         .arg(describe(err));
            return false;
        }
        done += n;
        if (progress && !progress(done, length)) {
            *error = QCoreApplication::translate("EspFlasher", "Cancelled.");
            return false;
        }
    }
    return true;
}

bool EspFlasher::write(quint32 address, const QByteArray& data, const Progress& progress,
                       QString* error)
{
    if (!m_session)
        return false;
    // The protocol takes whole 32-bit words; pad as an erased flash reads.
    QByteArray padded = data;
    while (padded.size() % 4)
        padded.append('\xFF');

    const quint32 block = kStubWriteBlock;   // open() always starts the stub
    esp_loader_flash_cfg_t cfg{};
    cfg.offset = address;
    cfg.image_size = static_cast<uint32_t>(padded.size());
    cfg.block_size = block;
    esp_loader_error_t err = esp_loader_flash_start(&m_session->loader, &cfg);
    for (qint64 done = 0; err == ESP_LOADER_SUCCESS && done < padded.size();) {
        const quint32 n = static_cast<quint32>(qMin<qint64>(block, padded.size() - done));
        err = esp_loader_flash_write(&m_session->loader, &cfg, padded.constData() + done, n);
        done += n;
        if (err == ESP_LOADER_SUCCESS && progress && !progress(done, padded.size())) {
            *error = QCoreApplication::translate(
                "EspFlasher", "Cancelled; the region at 0x%1 is incomplete.").arg(address, 0, 16);
            return false;
        }
    }
    if (err == ESP_LOADER_SUCCESS)
        err = esp_loader_flash_finish(&m_session->loader, &cfg);
    if (err != ESP_LOADER_SUCCESS) {
        *error = QCoreApplication::translate("EspFlasher", "Writing flash at 0x%1 failed: %2.")
                     .arg(address, 0, 16)
                     .arg(describe(err));
        return false;
    }
    return true;
}

bool EspFlasher::matches(quint32 address, const QByteArray& data)
{
    if (!m_session || data.isEmpty() || data.size() % 4)
        return false;
    // The library compares the digest as 32 lowercase hex characters.
    const QByteArray md5 = QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex();
    return esp_loader_flash_verify_known_md5(&m_session->loader, address,
                                             static_cast<uint32_t>(data.size()),
                                             reinterpret_cast<const uint8_t*>(md5.constData()))
           == ESP_LOADER_SUCCESS;
}

bool EspFlasher::restart(QString* error)
{
    if (!m_session)
        return false;
    if (m_session->chip != ESP32S3_CHIP) {
        *error = QCoreApplication::translate(
            "EspFlasher", "Press the device's RESET button to start its firmware.");
        close();
        return false;
    }
    esp_loader_t* l = &m_session->loader;
    // Clear "download mode forced over USB", then let the watchdog reset
    // the chip; with BOOT released it starts its firmware.
    uint32_t option1 = 0;
    esp_loader_error_t err = esp_loader_read_register(l, kS3RtcCntlOption1, &option1);
    if (err == ESP_LOADER_SUCCESS)
        err = esp_loader_write_register(l, kS3RtcCntlOption1, option1 & ~kS3ForceDownloadBoot);
    if (err == ESP_LOADER_SUCCESS)
        err = esp_loader_write_register(l, kS3WdtProtect, kS3WdtKey);
    if (err == ESP_LOADER_SUCCESS)
        err = esp_loader_write_register(l, kS3WdtConfig1, 2000);
    if (err == ESP_LOADER_SUCCESS)
        err = esp_loader_write_register(l, kS3WdtConfig0,
                                        (1u << 31) | (5u << 28) | (1u << 8) | 2u);
    if (err == ESP_LOADER_SUCCESS)
        esp_loader_write_register(l, kS3WdtProtect, 0);   // may not be answered: it resets
    close();
    if (err != ESP_LOADER_SUCCESS) {
        *error = QCoreApplication::translate(
                     "EspFlasher", "Could not restart the device (%1). Press its RESET button.")
                     .arg(describe(err));
        return false;
    }
    return true;
}

} // namespace AetherSDR
