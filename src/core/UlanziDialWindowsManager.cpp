#include <QtGlobal>
#if defined(Q_OS_WIN) && defined(HAVE_HIDAPI)

#include "UlanziDialWindowsManager.h"
#include "core/LogManager.h"

#include <QDebug>
#include <QTimer>

#include <hidapi/hidapi.h>

// Linux keycode numeric values (mirrored here so we don't need Linux
// headers).  The signature emitted is the canonical Linux-keycode-name
// string, so the dispatcher in MainWindow and the kPillSpecs table see
// the same strings regardless of host platform.
#define KEY_PLAYPAUSE    164
#define KEY_MUTE         113
#define KEY_PREVIOUSSONG 165
#define KEY_NEXTSONG     163
#define KEY_VOLUMEUP     115
#define KEY_VOLUMEDOWN   114
#define KEY_LEFTCTRL     29
#define KEY_RIGHTCTRL    97
#define KEY_V            47
#define KEY_C            46
#define KEY_Y            21
#define KEY_Z            44

namespace AetherSDR {

namespace {

// Map HID usage codes (consumer page 0x0C, keyboard page 0x07) into the
// Linux keycode integer space so we share the same signature output
// format with the Linux backend.
int hidConsumerToLinuxKey(unsigned short consumerUsage)
{
    switch (consumerUsage) {
        case 0xCD: return KEY_PLAYPAUSE;
        case 0xE2: return KEY_MUTE;
        case 0xB5: return KEY_NEXTSONG;
        case 0xB6: return KEY_PREVIOUSSONG;
        case 0xE9: return KEY_VOLUMEUP;
        case 0xEA: return KEY_VOLUMEDOWN;
        default:   return -1;
    }
}

int hidKbdToLinuxKey(unsigned char keycode)
{
    switch (keycode) {
        case 0x06: return KEY_C;   // HID Keyboard page 0x06 = C
        case 0x19: return KEY_V;   // 0x19 = V
        case 0x1C: return KEY_Y;   // 0x1C = Y
        case 0x1D: return KEY_Z;   // 0x1D = Z
        case 0xE0: return KEY_LEFTCTRL;
        case 0xE4: return KEY_RIGHTCTRL;
        default:   return -1;
    }
}

QString bareKeySignature(int keycode)
{
    switch (keycode) {
        case KEY_PLAYPAUSE:    return QStringLiteral("KEY_PLAYPAUSE");
        case KEY_MUTE:         return QStringLiteral("KEY_MUTE");
        case KEY_PREVIOUSSONG: return QStringLiteral("KEY_PREVIOUSSONG");
        case KEY_NEXTSONG:     return QStringLiteral("KEY_NEXTSONG");
        default:               return QStringLiteral("KEY_%1").arg(keycode);
    }
}

QString chordSignature(int keycode, bool withPreviousSong)
{
    QString letter;
    switch (keycode) {
        case KEY_V: letter = QStringLiteral("V"); break;
        case KEY_C: letter = QStringLiteral("C"); break;
        case KEY_Y: letter = QStringLiteral("Y"); break;
        case KEY_Z: letter = QStringLiteral("Z"); break;
        default:    letter = QStringLiteral("KEY_%1").arg(keycode); break;
    }
    if (withPreviousSong)
        return QStringLiteral("Ctrl+%1+KEY_PREVIOUSSONG").arg(letter);
    return QStringLiteral("Ctrl+%1").arg(letter);
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
    m_ctrlDown = false;
    m_lastNonModKey = -1;
    m_prevsongAlongsideCtrl = false;
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
            emitKeyTransition(KEY_LEFTCTRL, nowCtrl ? 1 : 0);
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
    if (linuxKey == KEY_VOLUMEUP) {
        if (value == 1) emit tuneSteps(+1);
        return;
    }
    if (linuxKey == KEY_VOLUMEDOWN) {
        if (value == 1) emit tuneSteps(-1);
        return;
    }
    if (linuxKey == KEY_LEFTCTRL || linuxKey == KEY_RIGHTCTRL) {
        m_ctrlDown = (value != 0);
        if (!m_ctrlDown) { m_lastNonModKey = -1; m_prevsongAlongsideCtrl = false; }
        return;
    }
    if (linuxKey == KEY_PREVIOUSSONG && m_ctrlDown && value == 1) {
        m_prevsongAlongsideCtrl = true;
        return;
    }
    if (linuxKey == KEY_PREVIOUSSONG && m_ctrlDown && value == 0) {
        return; // chord-window release; handled when Ctrl + non-mod release
    }
    if (!m_ctrlDown) {
        if (value == 1 || value == 0)
            emit buttonEvent(bareKeySignature(linuxKey), value);
        return;
    }
    // Chord window.
    if (value == 1) {
        m_lastNonModKey = linuxKey;
        emit buttonEvent(chordSignature(linuxKey, m_prevsongAlongsideCtrl), 1);
    } else if (value == 0 && linuxKey == m_lastNonModKey) {
        emit buttonEvent(chordSignature(linuxKey, m_prevsongAlongsideCtrl), 0);
        m_lastNonModKey = -1;
        m_prevsongAlongsideCtrl = false;
    }
}

} // namespace AetherSDR

#endif // Q_OS_WIN && HAVE_HIDAPI
