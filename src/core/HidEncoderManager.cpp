#ifdef HAVE_HIDAPI
#include "HidEncoderManager.h"
#include "core/AppSettings.h"
#include "core/LogManager.h"

#include <QDebug>
#include <algorithm>
#include <cstring>

namespace AetherSDR {

// HID logging now uses lcDevices from LogManager (shared with serial, FlexControl, MIDI)

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

    m_device = hid_open(vid, pid, nullptr);
    if (!m_device) {
        qCDebug(lcDevices) << "HidEncoderManager: failed to open"
                        << QString("0x%1:0x%2").arg(vid, 4, 16, QChar('0')).arg(pid, 4, 16, QChar('0'));
        return false;
    }

    hid_set_nonblocking(m_device, 1);

    m_parser = HidDeviceParser::create(vid, pid);
    if (!m_parser) {
        qCWarning(lcDevices) << "HidEncoderManager: no parser for"
                         << QString("0x%1:0x%2").arg(vid, 4, 16, QChar('0')).arg(pid, 4, 16, QChar('0'));
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
    if (m_device) {
        hid_close(m_device);
        m_device = nullptr;
    }
    m_parser.reset();
    if (!m_deviceName.isEmpty()) {
        qCDebug(lcDevices) << "HidEncoderManager: closed" << m_deviceName;
        m_deviceName.clear();
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

void HidEncoderManager::loadSettings()
{
    auto& s = AppSettings::instance();
    m_invertDirection = s.value("HidEncoderInvertDir", "False").toString() == "True";

    if (s.value("HidEncoderAutoDetect", "True").toString() == "True") {
        const auto* devices = HidDeviceParser::supportedDevices();
        int count = HidDeviceParser::supportedDeviceCount();
        for (int i = 0; i < count; ++i) {
            if (open(devices[i].vid, devices[i].pid))
                return;
        }
        // No device found — start hotplug timer to watch for connect
        m_hotplugTimer->start();
    }
}

} // namespace AetherSDR
#endif
