#ifdef HAVE_HIDAPI
#include "HidEncoderManager.h"
#include "core/AppSettings.h"
#include "core/LogManager.h"

#include <QDebug>
#include <QJsonObject>
#include <QJsonDocument>
#include <algorithm>
#include <cstring>

namespace AetherSDR {

// HID logging now uses lcDevices from LogManager (shared with serial, FlexControl, MIDI)

// RC-28 mapping is stored as one nested-JSON blob under "RC28Mapping"
// (Principle V / Principle XIV). Reads default-fill missing fields; writes
// regenerate the full object and persist atomically (single setValue+save).
QString HidEncoderManager::rc28MappingField(const QString& field, const QString& dflt)
{
    const QByteArray raw =
        AppSettings::instance().value("RC28Mapping", "{}").toString().toUtf8();
    const QJsonObject obj = QJsonDocument::fromJson(raw).object();
    const QJsonValue v = obj.value(field);
    return v.isString() ? v.toString() : dflt;
}

void HidEncoderManager::setRc28MappingField(const QString& field, const QString& value)
{
    auto& s = AppSettings::instance();
    QJsonObject obj =
        QJsonDocument::fromJson(s.value("RC28Mapping", "{}").toString().toUtf8()).object();
    obj.insert(field, value);
    s.setValue("RC28Mapping",
               QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    s.save();
}

HidEncoderManager::HidEncoderManager(QObject* parent)
    : QObject(parent)
{
    hid_init();

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(POLL_INTERVAL_MS);
    connect(m_pollTimer, &QTimer::timeout, this, &HidEncoderManager::poll);

    m_hotplugTimer = new QTimer(this);
    m_hotplugTimer->setInterval(HOTPLUG_INTERVAL_MS);
    connect(m_hotplugTimer, &QTimer::timeout, this, &HidEncoderManager::hotplugCheck);
}

HidEncoderManager::~HidEncoderManager()
{
    close();
    hid_exit();
}

QString HidEncoderManager::detectDevice()
{
    const auto* devices = HidDeviceParser::supportedDevices();
    int count = HidDeviceParser::supportedDeviceCount();

    for (int i = 0; i < count; ++i) {
        auto* info = hid_enumerate(devices[i].vid, devices[i].pid);
        if (info) {
            QString name = devices[i].name;
            hid_free_enumeration(info);
            return name;
        }
    }
    return {};
}

bool HidEncoderManager::open(uint16_t vid, uint16_t pid)
{
    if (m_device) close();

    // Block if more than one RC-28-compatible device is connected — interleaved
    // events from two encoders would produce unpredictable tuning behaviour.
    // macOS reports each HID usage collection as a separate enumeration entry, so
    // counting raw entries would over-count a single device. We group by a stable
    // physical-device key: the USB serial number when present, otherwise the
    // hidapi path. The RC-28 exposes no serial, but all usage-collection entries
    // of one physical interface share the same path, while two separate devices
    // get distinct paths — so the path fallback distinguishes them correctly.
    if (isRC28CompatibleId(vid, pid)) {
        bool multiplePhysical = false;
        QString firstKey;
        bool firstSeen = false;
        if (auto* info = hid_enumerate(vid, pid)) {
            for (auto* cur = info; cur; cur = cur->next) {
                const QString key = (cur->serial_number && cur->serial_number[0] != L'\0')
                    ? QStringLiteral("sn:") + QString::fromWCharArray(cur->serial_number)
                    : QStringLiteral("path:") + QString::fromLatin1(cur->path ? cur->path : "");
                if (!firstSeen) {
                    firstKey  = key;
                    firstSeen = true;
                } else if (key != firstKey) {
                    multiplePhysical = true;
                    break;
                }
            }
            hid_free_enumeration(info);
        }
        if (multiplePhysical) {
            const auto* devices = HidDeviceParser::supportedDevices();
            int count = HidDeviceParser::supportedDeviceCount();
            QString name;
            for (int i = 0; i < count; ++i) {
                if (devices[i].vid == vid && devices[i].pid == pid) {
                    name = devices[i].name;
                    break;
                }
            }
            // open() is retried every hotplug tick while two devices remain, so
            // warn + emit only on the transition into the blocked state.
            if (!m_multipleDetected.load(std::memory_order_acquire)) {
                // Write the name before the release store so the main thread
                // sees a valid QString when it reads m_multipleDetected as true.
                m_blockedDeviceName = name;
                m_multipleDetected.store(true, std::memory_order_release);
                qCWarning(lcDevices) << "HidEncoderManager: multiple" << name
                                     << "devices detected — blocking until only one is present";
                emit multipleDevicesDetected(name);
            }
            return false;
        }
        m_blockedDeviceName.clear();
        m_multipleDetected.store(false, std::memory_order_release);
    }

    m_device = hid_open(vid, pid, nullptr);
    if (!m_device) {
        qCDebug(lcDevices) << "HidEncoderManager: failed to open"
                        << QString("0x%1:0x%2").arg(vid, 4, 16, QChar('0')).arg(pid, 4, 16, QChar('0'));
        return false;
    }

    hid_set_nonblocking(m_device, 1);

    // Capture device info via enumerate — works on all hidapi versions unlike
    // hid_get_device_info() which requires >= 0.13.0.  (#3323)
    if (auto* info = hid_enumerate(vid, pid)) {
        m_devicePath   = QString::fromLatin1(info->path ? info->path : "");
        m_serialNumber = info->serial_number
            ? QString::fromWCharArray(info->serial_number) : QString{};
        m_releaseNumber = info->release_number;
        hid_free_enumeration(info);
    }

    m_parser = HidDeviceParser::create(vid, pid);
    if (!m_parser) {
        qCWarning(lcDevices) << "HidEncoderManager: no parser for"
                         << QString("0x%1:0x%2").arg(vid, 4, 16, QChar('0')).arg(pid, 4, 16, QChar('0'));
        m_devicePath.clear();
        m_serialNumber.clear();
        m_releaseNumber = 0;
        hid_close(m_device);
        m_device = nullptr;
        return false;
    }

    m_openVid = vid;
    m_openPid = pid;
    m_hotplugTimer->stop();

    // Find device name
    const auto* devices = HidDeviceParser::supportedDevices();
    int count = HidDeviceParser::supportedDeviceCount();
    for (int i = 0; i < count; ++i) {
        if (devices[i].vid == vid && devices[i].pid == pid) {
            m_deviceName = devices[i].name;
            break;
        }
    }

    m_pollTimer->start();

    qCDebug(lcDevices) << "HidEncoderManager: opened" << m_deviceName
                    << QString("0x%1:0x%2").arg(vid, 4, 16, QChar('0')).arg(pid, 4, 16, QChar('0'));
    emit connectionChanged(true, m_deviceName);
    return true;
}

void HidEncoderManager::close()
{
    m_pollTimer->stop();
    m_hotplugTimer->stop();
    if (m_device) {
        // Extinguish RC-28 LEDs on clean close. hid_write may return EIO if
        // the device was surprise-disconnected; that is safe to ignore here.
        setRC28Leds(RC28_LEDS_OFF);
        hid_close(m_device);
        m_device = nullptr;
    }
    m_parser.reset();
    if (!m_deviceName.isEmpty()) {
        qCDebug(lcDevices) << "HidEncoderManager: closed" << m_deviceName;
        m_deviceName.clear();
        m_devicePath.clear();
        m_serialNumber.clear();
        m_releaseNumber = 0;
        emit connectionChanged(false, {});
    }
}

void HidEncoderManager::poll()
{
    if (!m_device || !m_parser) return;

    // Read all pending reports
    while (true) {
        int res = hid_read(m_device, m_buf, m_parser->reportSize());
        if (res < 0) {
            // Device disconnected
            qCDebug(lcDevices) << "HidEncoderManager: device disconnected, starting hotplug";
            close();
            m_hotplugTimer->start();
            return;
        }
        if (res == 0) break;  // no more data

        auto event = m_parser->parse(m_buf, static_cast<size_t>(res));
        switch (event.type) {
        case HidEvent::Rotate:
            emit tuneSteps(event.encoderIndex, m_invertDirection ? -event.steps : event.steps);
            break;
        case HidEvent::Button:
            emit buttonPressed(event.button, event.action);
            break;
        case HidEvent::None:
            break;
        }
    }
}

void HidEncoderManager::hotplugCheck()
{
    if (m_device) {
        m_hotplugTimer->stop();
        return;
    }
    if (m_openVid && m_openPid) {
        if (open(m_openVid, m_openPid))
            m_hotplugTimer->stop();
        return;
    }
    // No VID/PID recorded: device was never opened (started without encoder
    // attached). Scan all supported devices so a late-connect is picked up.
    const auto* devices = HidDeviceParser::supportedDevices();
    int count = HidDeviceParser::supportedDeviceCount();
    for (int i = 0; i < count; ++i) {
        if (open(devices[i].vid, devices[i].pid)) {
            m_hotplugTimer->stop();
            return;
        }
    }
}

void HidEncoderManager::setKeyImages(const QVector<QByteArray>& jpegImages)
{
    for (int i = 0; i < jpegImages.size(); ++i)
        setKeyImage(i, jpegImages[i]);
}

void HidEncoderManager::setKeyImage(int key, const QByteArray& jpegData)
{
    if (!m_device || !isStreamDeckPlus()) return;

    // StreamDeck+ LCD image write: 1024-byte feature reports (report ID 0x02),
    // command 0x07 (set key image). Protocol verified against python-elgato-streamdeck.
    constexpr int PACKET_SIZE  = 1024;
    constexpr int HEADER_SIZE  = 8;
    constexpr int PAYLOAD_SIZE = PACKET_SIZE - HEADER_SIZE;

    const int totalBytes = jpegData.size();
    int offset     = 0;
    int pageNumber = 0;

    while (offset < totalBytes) {
        uint8_t pkt[PACKET_SIZE] = {};
        const int chunkLen = std::min(PAYLOAD_SIZE, totalBytes - offset);
        const bool isLast  = (offset + chunkLen >= totalBytes);

        pkt[0] = 0x02;   // report ID
        pkt[1] = 0x07;   // command: set key image
        pkt[2] = static_cast<uint8_t>(key);
        pkt[3] = isLast ? 1 : 0;
        pkt[4] = static_cast<uint8_t>(chunkLen & 0xFF);
        pkt[5] = static_cast<uint8_t>((chunkLen >> 8) & 0xFF);
        pkt[6] = static_cast<uint8_t>(pageNumber & 0xFF);
        pkt[7] = static_cast<uint8_t>((pageNumber >> 8) & 0xFF);
        std::memcpy(pkt + HEADER_SIZE, jpegData.constData() + offset, chunkLen);

        // Bail on write failure so we don't spin through the remaining packets
        // writing into a dead handle.  The next poll() will catch the bad
        // handle via hid_read() < 0 and trigger close() + hotplug reopen,
        // which correlates the user-visible "deck went blank" with logs. (#3248)
        const int written = hid_write(m_device, pkt, PACKET_SIZE);
        if (written < 0) {
            qCWarning(lcDevices) << "HidEncoderManager::setKeyImage: hid_write failed"
                                 << "key=" << key
                                 << "page=" << pageNumber
                                 << "— device disconnected? Will retry on hotplug.";
            return;
        }

        offset     += chunkLen;
        pageNumber++;
    }
}

void HidEncoderManager::setTouchscreenImage(const QByteArray& jpegData,
                                             int x_pos, int y_pos,
                                             int width, int height)
{
    if (!m_device || !isStreamDeckPlus()) return;

    // Touchscreen write: 1024-byte packets, 16-byte header, command 0x0c.
    // Protocol verified against python-elgato-streamdeck StreamDeckPlus.set_touchscreen_image().
    constexpr int PACKET_SIZE  = 1024;
    constexpr int HEADER_SIZE  = 16;
    constexpr int PAYLOAD_SIZE = PACKET_SIZE - HEADER_SIZE;

    const int totalBytes = jpegData.size();
    int offset     = 0;
    int pageNumber = 0;

    while (offset < totalBytes) {
        uint8_t pkt[PACKET_SIZE] = {};
        const int chunkLen = std::min(PAYLOAD_SIZE, totalBytes - offset);
        const bool isLast  = (offset + chunkLen >= totalBytes);

        pkt[0]  = 0x02;
        pkt[1]  = 0x0c;
        pkt[2]  = static_cast<uint8_t>(x_pos & 0xff);
        pkt[3]  = static_cast<uint8_t>((x_pos >> 8) & 0xff);
        pkt[4]  = static_cast<uint8_t>(y_pos & 0xff);
        pkt[5]  = static_cast<uint8_t>((y_pos >> 8) & 0xff);
        pkt[6]  = static_cast<uint8_t>(width & 0xff);
        pkt[7]  = static_cast<uint8_t>((width >> 8) & 0xff);
        pkt[8]  = static_cast<uint8_t>(height & 0xff);
        pkt[9]  = static_cast<uint8_t>((height >> 8) & 0xff);
        pkt[10] = isLast ? 1 : 0;
        pkt[11] = static_cast<uint8_t>(pageNumber & 0xff);
        pkt[12] = static_cast<uint8_t>((pageNumber >> 8) & 0xff);
        pkt[13] = static_cast<uint8_t>(chunkLen & 0xff);
        pkt[14] = static_cast<uint8_t>((chunkLen >> 8) & 0xff);
        pkt[15] = 0x00;
        std::memcpy(pkt + HEADER_SIZE, jpegData.constData() + offset, chunkLen);

        // Same bail-on-failure pattern as setKeyImage above. (#3248)
        const int written = hid_write(m_device, pkt, PACKET_SIZE);
        if (written < 0) {
            qCWarning(lcDevices) << "HidEncoderManager::setTouchscreenImage: hid_write failed"
                                 << "page=" << pageNumber
                                 << "— device disconnected? Will retry on hotplug.";
            return;
        }

        offset     += chunkLen;
        pageNumber++;
    }
}

void HidEncoderManager::setRC28Leds(uint8_t ledByte)
{
    if (!m_device || !isRC28Compatible()) return;
    // Output report: [0x00=reportID, 0x01=cmd, ledByte, zeros...], 33 bytes total.
    // Format verified against FlexRC-28 Node.js driver (_sendLED) and
    // wfview src/usbcontroller.cpp (RC28 featureLEDControl path).
    // Active-low: bit0=TX, bit1=F1, bit2=F2, bit3=LINK; 0x0F = all off.
    uint8_t report[33] = {};
    report[0] = 0x00;
    report[1] = 0x01;
    report[2] = ledByte;
    hid_write(m_device, report, sizeof(report));
}

void HidEncoderManager::loadSettings()
{
    auto& s = AppSettings::instance();
    m_invertDirection = s.value("HidEncoderInvertDir", "False").toString() == "True";

    // Callers (MainWindow startup + Preferences OK) gate on HidEncoderEnabled, so
    // loadSettings() always scans for a device when called.  The isOpen() guard
    // makes repeated calls from Preferences idempotent: invert-dir is refreshed
    // above, but we skip the scan+open cycle if the device is already connected.
    // Replacing the old HidEncoderAutoDetect check prevents users who had that
    // flag set to "False" from getting stuck in a "can't re-enable" state. (#3323)
    if (isOpen()) return;

    const auto* devices = HidDeviceParser::supportedDevices();
    int count = HidDeviceParser::supportedDeviceCount();
    for (int i = 0; i < count; ++i) {
        if (open(devices[i].vid, devices[i].pid))
            return;
    }
    // No device found — start hotplug timer to watch for connect
    m_hotplugTimer->start();
}

} // namespace AetherSDR
#endif
