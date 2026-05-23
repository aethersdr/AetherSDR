#ifdef HAVE_HIDAPI
#include "HidEncoderManager.h"
#include "core/AppSettings.h"
#include "core/LogManager.h"

#include <QDebug>

namespace MasterSDR {

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
            emit tuneSteps(m_invertDirection ? -event.steps : event.steps);
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

} // namespace MasterSDR
#endif
