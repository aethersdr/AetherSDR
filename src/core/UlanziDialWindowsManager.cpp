#include <QtGlobal>
#if defined(Q_OS_WIN) && defined(HAVE_HIDAPI)

#include "UlanziDialWindowsManager.h"
#include "core/LogManager.h"
#include "core/UlanziChordDecoder.h"

#include <QDebug>
#include <QTimer>

#include <hidapi/hidapi.h>

namespace AetherSDR {

namespace {

// Map HID usage codes (consumer page 0x0C, keyboard page 0x07) into the
// Linux keycode integer space so we share the same signature output
// format with the Linux backend.
int hidConsumerToLinuxKey(unsigned short consumerUsage)
{
    switch (consumerUsage) {
        case 0xCD: return UlanziKey::PlayPause;
        case 0xE2: return UlanziKey::Mute;
        case 0xB5: return UlanziKey::NextSong;
        case 0xB6: return UlanziKey::PreviousSong;
        case 0xE9: return UlanziKey::VolumeUp;
        case 0xEA: return UlanziKey::VolumeDown;
        default:   return -1;
    }
}

int hidKbdToLinuxKey(unsigned char keycode)
{
    switch (keycode) {
        case 0x06: return UlanziKey::C;   // HID Keyboard page 0x06 = C
        case 0x19: return UlanziKey::V;   // 0x19 = V
        case 0x1C: return UlanziKey::Y;   // 0x1C = Y
        case 0x1D: return UlanziKey::Z;   // 0x1D = Z
        case 0xE0: return UlanziKey::LeftCtrl;
        case 0xE4: return UlanziKey::RightCtrl;
        default:   return -1;
    }
}

constexpr const wchar_t* kProductMatch = L"Ulanzi Dial";

} // namespace

UlanziDialWindowsManager::UlanziDialWindowsManager(QObject* parent)
    : QObject(parent)
{
    hid_init();
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(POLL_INTERVAL_MS);
    connect(m_pollTimer, &QTimer::timeout, this, &UlanziDialWindowsManager::poll);

    m_hotplugTimer = new QTimer(this);
    m_hotplugTimer->setInterval(HOTPLUG_INTERVAL_MS);
    connect(m_hotplugTimer, &QTimer::timeout,
            this, &UlanziDialWindowsManager::hotplugCheck);
}

UlanziDialWindowsManager::~UlanziDialWindowsManager()
{
    stop();
    hid_exit();
}

void UlanziDialWindowsManager::start()
{
    if (rescan()) {
        m_pollTimer->start();
        emit connectionChanged(true, m_deviceName);
    }
    m_hotplugTimer->start();
}

void UlanziDialWindowsManager::stop()
{
    m_pollTimer->stop();
    m_hotplugTimer->stop();
    closeAll();
}

bool UlanziDialWindowsManager::rescan()
{
    closeAll();
    struct hid_device_info* infos = hid_enumerate(0, 0);
    for (auto* info = infos; info; info = info->next) {
        if (!info->product_string) continue;
        if (wcsstr(info->product_string, kProductMatch) == nullptr) continue;
        hid_device* h = hid_open_path(info->path);
        if (!h) continue;
        hid_set_nonblocking(h, 1);
        OpenDevice dev;
        dev.handle = h;
        dev.path = QString::fromUtf8(info->path);
        dev.productString = QString::fromWCharArray(info->product_string);
        dev.lastReport.resize(0);
        m_devices.append(dev);
    }
    hid_free_enumeration(infos);
    if (m_devices.isEmpty()) { m_deviceName.clear(); return false; }
    m_deviceName = m_devices.first().productString;
    return true;
}

void UlanziDialWindowsManager::closeAll()
{
    for (auto& d : m_devices) {
        if (d.handle) {
            hid_close(static_cast<hid_device*>(d.handle));
            d.handle = nullptr;
        }
    }
    m_devices.clear();
    if (!m_deviceName.isEmpty()) {
        const QString name = m_deviceName;
        m_deviceName.clear();
        emit connectionChanged(false, name);
    }
    m_decoder.reset();
}

void UlanziDialWindowsManager::hotplugCheck()
{
    if (!m_devices.isEmpty()) return;
    if (rescan()) {
        m_pollTimer->start();
        emit connectionChanged(true, m_deviceName);
    }
}

void UlanziDialWindowsManager::poll()
{
    unsigned char buf[64];
    bool anyAlive = false;
    for (auto it = m_devices.begin(); it != m_devices.end(); ) {
        if (!it->handle) { it = m_devices.erase(it); continue; }
        int n = hid_read(static_cast<hid_device*>(it->handle), buf, sizeof(buf));
        if (n < 0) {
            qCDebug(lcDevices) << "UlanziDialWindowsManager: read failed on" << it->path;
            hid_close(static_cast<hid_device*>(it->handle));
            it->handle = nullptr;
            it = m_devices.erase(it);
            continue;
        }
        if (n > 0) handleReport(*it, buf, n);
        anyAlive = true;
        ++it;
    }
    if (!anyAlive) {
        const QString name = m_deviceName;
        m_deviceName.clear();
        m_pollTimer->stop();
        emit connectionChanged(false, name);
    }
}

void UlanziDialWindowsManager::handleReport(OpenDevice& dev,
                                            const unsigned char* data, int len)
{
    // Heuristic: discriminate between Consumer (Page 0x0C) and Keyboard
    // (Page 0x07) reports by length + content.  Real HID stacks tag with
    // a report id in byte 0; without parsing the descriptor we use the
    // typical shapes Ulanzi Dial firmware emits.
    //
    // Consumer page report — 2-byte usage code in little-endian:
    //   [report_id?] [usage_lo] [usage_hi]   (3 bytes total typical)
    // Keyboard report (8 bytes):
    //   [modifier_mask] [reserved] [key1] [key2] ... [key6]
    QVector<unsigned char> cur(data, data + len);
    QVector<unsigned char> prev = dev.lastReport;
    dev.lastReport = cur;

    if (len == 8) {
        // Standard keyboard report.
        const unsigned char mod = data[0];
        const unsigned char prevMod = prev.size() == 8 ? prev[0] : 0;
        // Ctrl transitions.
        const bool nowCtrl  = (mod      & 0x01) || (mod      & 0x10);
        const bool prevCtrl = (prevMod & 0x01) || (prevMod & 0x10);
        if (nowCtrl != prevCtrl)
            emitKeyTransition(UlanziKey::LeftCtrl, nowCtrl ? 1 : 0);
        // Pressed-keys diff: keys 2..7.
        auto contains = [](const QVector<unsigned char>& r, unsigned char k) -> bool {
            for (int i = 2; i < r.size() && i < 8; ++i)
                if (r[i] == k && k != 0) return true;
            return false;
        };
        for (int i = 2; i < len && i < 8; ++i) {
            const unsigned char k = data[i];
            if (k == 0 || contains(prev, k)) continue;
            const int linuxKey = hidKbdToLinuxKey(k);
            if (linuxKey >= 0) emitKeyTransition(linuxKey, 1);
        }
        for (int i = 2; i < prev.size() && i < 8; ++i) {
            const unsigned char k = prev[i];
            if (k == 0 || contains(cur, k)) continue;
            const int linuxKey = hidKbdToLinuxKey(k);
            if (linuxKey >= 0) emitKeyTransition(linuxKey, 0);
        }
    } else if (len >= 2) {
        // Consumer-page-ish report; treat first usage as the active key.
        const unsigned short usage =
            static_cast<unsigned short>(data[0]) |
            (static_cast<unsigned short>(data[1]) << 8);
        const int linuxKey = hidConsumerToLinuxKey(usage);
        if (linuxKey < 0) return;
        // Synthesise press + release (consumer-control reports usually
        // appear once per dial detent; nonzero means pressed).
        emitKeyTransition(linuxKey, 1);
        emitKeyTransition(linuxKey, 0);
    }
}

void UlanziDialWindowsManager::emitKeyTransition(int linuxKey, int value)
{
    for (const auto& out : m_decoder.feed(linuxKey, value)) {
        if (out.kind == UlanziChordDecoder::Event::Kind::Tune)
            emit tuneSteps(out.tuneSteps);
        else
            emit buttonEvent(out.signature, out.action);
    }
}

} // namespace AetherSDR

#endif // Q_OS_WIN && HAVE_HIDAPI
