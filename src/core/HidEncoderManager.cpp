#ifdef HAVE_HIDAPI
#include "HidEncoderManager.h"
#include "core/AppSettings.h"
#include "core/LogManager.h"

#include <QDebug>
#include <QJsonObject>
#include <QJsonDocument>
#include <algorithm>
#include <cstring>

// ── TMate 2 display helpers ────────────────────────────────────────────────
//
// Digit encoding for the TMate 2 LCD.  Tables and byte layout are derived
// from OpenTMate2Lib (D:\Code\OpenTMate2Lib), which reverse-engineered the
// protocol from TMATE2_DLL.dll via USBPcap captures (2026-06-05).
//
// Main 9-digit display (freq in Hz):
//   Each digit d (1=units, 9=100 MHz) occupies two LCDVector bytes:
//     high = 22 - 2*d   bits: A=0x01 B=0x02 C=0x04
//     low  = 21 - 2*d   bits: F=0x01 G=0x02 E=0x04 D=0x08
//
// Small 3-digit display (S-meter / TX power):
//   Each digit d (1=units, 3=hundreds) occupies two LCDVector bytes:
//     high = 21 + 2*d   bits: A=0x80 B=0x40 C=0x20 D=0x10
//     low  = 22 + 2*d   bits: E=0x20 F=0x80 G=0x40
//   (bit ordering is reversed vs. main display — hardware quirk)

namespace {

static const uint8_t kMainHigh[10] = {
    0x07, 0x06, 0x03, 0x07, 0x06, 0x05, 0x05, 0x07, 0x07, 0x07
};
static const uint8_t kMainLow[10] = {
    0x0D, 0x00, 0x0E, 0x0A, 0x03, 0x0B, 0x0F, 0x00, 0x0F, 0x0B
};
static const uint8_t kSmallHigh[10] = {
    0xF0, 0x60, 0xD0, 0xF0, 0x60, 0xB0, 0xB0, 0xE0, 0xF0, 0xF0
};
static const uint8_t kSmallLow[10] = {
    0xA0, 0x00, 0x60, 0x40, 0xC0, 0xC0, 0xE0, 0x00, 0xE0, 0xC0
};

// Indicator segment lookup: {byte_index, bitmask}.
// Derived from USB captures (session_20260605_051711/segments_*.csv).
// Byte range 0-31 only; indicator bits never overlap with digit A-G bits.
struct SegEntry { uint8_t byte; uint8_t mask; };
static constexpr SegEntry kSeg_RX        = {  0, 0x04 };
static constexpr SegEntry kSeg_TX        = {  0, 0x08 };
static constexpr SegEntry kSeg_S         = {  0, 0x10 };
static constexpr SegEntry kSeg_VOL       = {  1, 0x80 };
static constexpr SegEntry kSeg_SMETER_LINE = { 2, 0x01 };
static constexpr SegEntry kSeg_SMETER_DB_MINUS = {28, 0x10 };
static constexpr SegEntry kSeg_DIG_PLUS  = { 21, 0x04 };
static constexpr SegEntry kSeg_DIG_MINUS = { 21, 0x08 };
static constexpr SegEntry kSeg_DSB       = { 21, 0x10 };
static constexpr SegEntry kSeg_FM        = { 21, 0x20 };
static constexpr SegEntry kSeg_USB       = { 21, 0x40 };
static constexpr SegEntry kSeg_SAM       = { 21, 0x80 };
static constexpr SegEntry kSeg_DIG       = { 22, 0x02 };
static constexpr SegEntry kSeg_DBM       = { 22, 0x10 };
static constexpr SegEntry kSeg_CW        = { 22, 0x20 };
static constexpr SegEntry kSeg_LSB       = { 22, 0x40 };
static constexpr SegEntry kSeg_AM        = { 22, 0x80 };
static constexpr SegEntry kSeg_DOT1      = {  9, 0x10 };  // after digit 3 (kHz/Hz)
static constexpr SegEntry kSeg_DOT2      = { 15, 0x10 };  // after digit 6 (MHz/kHz)
static constexpr SegEntry kSeg_HZ        = { 23, 0x01 };
static constexpr SegEntry kSeg_W         = { 20, 0x20 };
static constexpr SegEntry kSeg_RIT       = { 13, 0x10 };
static constexpr SegEntry kSeg_XIT       = { 14, 0x10 };
// 15-segment S-meter bargraph (BAR1=weakest, BAR15=strongest)
static constexpr SegEntry kSMeterBars[15] = {
    { 1, 0x08 }, { 1, 0x04 }, { 1, 0x02 }, // BAR1-3
    { 31, 0x80 }, { 31, 0x40 }, { 31, 0x20 }, { 31, 0x10 }, // BAR4-7
    { 30, 0x10 }, { 30, 0x20 }, { 30, 0x40 }, { 30, 0x80 }, // BAR8-11
    { 29, 0x80 }, { 29, 0x40 }, { 29, 0x20 }, { 29, 0x10 }, // BAR12-15
};

// Set or clear one segment bit in the LCDVector.
static void tmate2Seg(uint8_t* v, SegEntry s, bool on)
{
    if (on) v[s.byte] |= s.mask;
    else    v[s.byte] &= static_cast<uint8_t>(~s.mask);
}

// Convert dBm to S-meter bargraph bar count (0-15).
//   S1(-121 dBm)=1, S9(-73 dBm)=9; above S9: +10 dB per bar up to S9+60 dB(=15).
static int smeterBars(float dbm)
{
    if (dbm < -121.0f) return 0;
    if (dbm <= -73.0f) return static_cast<int>((dbm + 121.0f) / 6.0f) + 1;
    return std::min(15, 9 + static_cast<int>((dbm + 73.0f) / 10.0f));
}

static void applyModeSegs(uint8_t* lcd, const QString& mode)
{
    tmate2Seg(lcd, kSeg_USB, false);
    tmate2Seg(lcd, kSeg_LSB, false);
    tmate2Seg(lcd, kSeg_AM,  false);
    tmate2Seg(lcd, kSeg_SAM, false);
    tmate2Seg(lcd, kSeg_DSB, false);
    tmate2Seg(lcd, kSeg_FM,  false);
    tmate2Seg(lcd, kSeg_CW,  false);
    tmate2Seg(lcd, kSeg_DIG, false);
    tmate2Seg(lcd, kSeg_DIG_PLUS, false);
    tmate2Seg(lcd, kSeg_DIG_MINUS, false);

    if      (mode == "USB")               tmate2Seg(lcd, kSeg_USB, true);
    else if (mode == "LSB")               tmate2Seg(lcd, kSeg_LSB, true);
    else if (mode == "AM")                tmate2Seg(lcd, kSeg_AM,  true);
    else if (mode == "SAM")               tmate2Seg(lcd, kSeg_SAM, true);
    else if (mode == "DSB")               tmate2Seg(lcd, kSeg_DSB, true);
    else if (mode == "FM"  || mode == "DFM")  tmate2Seg(lcd, kSeg_FM,  true);
    else if (mode == "CW"  || mode == "CWL" || mode == "CWU") tmate2Seg(lcd, kSeg_CW, true);
    else if (mode == "DIGU") {
        tmate2Seg(lcd, kSeg_DIG, true);
        tmate2Seg(lcd, kSeg_DIG_PLUS, true);
    } else if (mode == "DIGL") {
        tmate2Seg(lcd, kSeg_DIG, true);
        tmate2Seg(lcd, kSeg_DIG_MINUS, true);
    } else if (mode == "RTTY" || mode.startsWith("DIG")) {
        tmate2Seg(lcd, kSeg_DIG, true);
    }
}

// LCDVector byte offsets (matches OpenTMate2Lib and Delphi LCD_SEGMENT_DEF)
static constexpr int kTM2_LED      = 32;
static constexpr int kTM2_BL_R     = 33;
static constexpr int kTM2_BL_G     = 34;
static constexpr int kTM2_BL_B     = 35;
static constexpr int kTM2_CONTRAST = 36;
static constexpr int kTM2_REFRESH  = 37;
static constexpr int kTM2_SPEED1   = 38;
static constexpr int kTM2_SPEED2   = 39;
static constexpr int kTM2_SPEED3   = 40;
static constexpr int kTM2_THR12    = 41;
static constexpr int kTM2_THR23    = 42;
static constexpr int kTM2_EVAL     = 43;

// Write frequency (Hz) to bytes 3..20 of the LCDVector.
// Only the seven A-G segment bits per digit are touched; indicator bits that
// share those bytes (underlines, DRV, NR2, …) are preserved.
static void tmate2WriteMainDisplay(uint8_t* v, uint32_t hz)
{
    if (hz > 999999999u) hz = 999999999u;
    for (int d = 1; d <= 9; ++d) {
        uint8_t hi = static_cast<uint8_t>(22 - 2 * d);
        uint8_t lo = static_cast<uint8_t>(21 - 2 * d);
        v[hi] &= 0xF8u;
        v[lo] &= 0xF0u;
        if (hz == 0u && d > 1) continue;
        v[hi] |= kMainHigh[hz % 10u];
        v[lo] |= kMainLow [hz % 10u];
        hz /= 10u;
    }
}

// Write a value (mod 1000) to bytes 23..28 of the LCDVector.
static void tmate2WriteSmallDisplay(uint8_t* v, uint32_t val)
{
    val %= 1000u;
    for (int d = 1; d <= 3; ++d) {
        uint8_t hi = static_cast<uint8_t>(21 + 2 * d);
        uint8_t lo = static_cast<uint8_t>(22 + 2 * d);
        v[hi] &= 0x0Fu;
        v[lo] &= 0x1Fu;
        if (val == 0u && d > 1) continue;
        v[hi] |= kSmallHigh[val % 10u];
        v[lo] |= kSmallLow [val % 10u];
        val /= 10u;
    }
}

} // namespace

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

    // Initialise TMate 2 LCDVector state and push a blank display.
    // Timing defaults from OpenTMate2Lib protocol docs (captures 2026-06-05).
    // Backlight is restored from AppSettings (saved by Preferences dialog).
    if (isTMate2()) {
        std::memset(m_lcdVector, 0, sizeof(m_lcdVector));
        // Matches the original Delphi app / TMate2Probe captures. 0x28 here
        // overdrives the LCD on some units and can make the display unreadable.
        m_lcdVector[kTM2_CONTRAST] = 0x00;
        m_lcdVector[kTM2_REFRESH]  = 0x28;
        m_lcdVector[kTM2_SPEED1]   = 0x01;
        m_lcdVector[kTM2_SPEED2]   = 0x05;
        m_lcdVector[kTM2_SPEED3]   = 0x0A;
        m_lcdVector[kTM2_THR12]    = 0x0F;
        m_lcdVector[kTM2_THR23]    = 0x19;
        m_lcdVector[kTM2_EVAL]     = 0x0A;
        const auto& s = AppSettings::instance();
        m_lcdVector[kTM2_BL_R] = static_cast<uint8_t>(s.value("TMate2BacklightR", "0").toInt());
        m_lcdVector[kTM2_BL_G] = static_cast<uint8_t>(s.value("TMate2BacklightG", "50").toInt());
        m_lcdVector[kTM2_BL_B] = static_cast<uint8_t>(s.value("TMate2BacklightB", "255").toInt());
        // Always-on static indicators for a frequency display
        tmate2Seg(m_lcdVector, kSeg_SMETER_LINE, true);
        tmate2Seg(m_lcdVector, kSeg_DOT1, true);
        tmate2Seg(m_lcdVector, kSeg_DOT2, true);
        tmate2Seg(m_lcdVector, kSeg_HZ,   true);
        tmate2Seg(m_lcdVector, kSeg_VOL,  true);
        sendTMate2();
    }

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
        // Extinguish RC-28 LEDs / TMate 2 backlight on clean close.
        // hid_write may return EIO on surprise-disconnect; safe to ignore.
        setRC28Leds(RC28_LEDS_OFF);
        setTMate2Backlight(0, 0, 0);
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

void HidEncoderManager::sendTMate2()
{
    // Build the 64-byte output report from the persistent LCDVector state.
    // Bytes 0-43 = LCDVector (segments, status, backlight, timing).
    // Bytes 44-63 = zero padding required by the protocol.
    uint8_t report[64] = {};
    std::memcpy(report, m_lcdVector, sizeof(m_lcdVector));

    // hidapi write buffers include a leading report ID byte.  The TMate 2 OUT
    // payload captured on USB is the 64-byte LCDVector report itself, so prepend
    // report ID 0 to avoid shifting the vector left by one byte on Windows.
    uint8_t hidReport[65] = {};
    std::memcpy(hidReport + 1, report, sizeof(report));
    const int written = hid_write(m_device, hidReport, sizeof(hidReport));
    if (written < 0) {
        qCWarning(lcDevices) << "HidEncoderManager::sendTMate2: hid_write failed";
    }
}

void HidEncoderManager::setTMate2Backlight(uint8_t r, uint8_t g, uint8_t b)
{
    if (!m_device || !isTMate2()) return;
    m_lcdVector[kTM2_BL_R] = r;
    m_lcdVector[kTM2_BL_G] = g;
    m_lcdVector[kTM2_BL_B] = b;
    sendTMate2();
}

void HidEncoderManager::setTMate2Status(uint8_t led_byte)
{
    if (!m_device || !isTMate2()) return;
    m_lcdVector[kTM2_LED] = led_byte;
    sendTMate2();
}

void HidEncoderManager::setTMate2Display(uint32_t freq_hz, uint32_t small_val)
{
    if (!m_device || !isTMate2()) return;
    tmate2WriteMainDisplay(m_lcdVector, freq_hz);
    tmate2WriteSmallDisplay(m_lcdVector, small_val);
    sendTMate2();
}

void HidEncoderManager::setTMate2Indicators(bool tx, const QString& mode,
                                             float smeter_dbm, bool rit, bool xit)
{
    if (!m_device || !isTMate2()) return;

    // RX / TX
    tmate2Seg(m_lcdVector, kSeg_RX, !tx);
    tmate2Seg(m_lcdVector, kSeg_TX,  tx);
    tmate2Seg(m_lcdVector, kSeg_W,   tx);
    tmate2Seg(m_lcdVector, kSeg_DBM, !tx);
    tmate2Seg(m_lcdVector, kSeg_S,   !tx);
    tmate2Seg(m_lcdVector, kSeg_VOL, true);
    tmate2Seg(m_lcdVector, kSeg_SMETER_LINE, true);
    tmate2Seg(m_lcdVector, kSeg_SMETER_DB_MINUS, !tx && smeter_dbm < 0.0f);

    // Static frequency-display decorations (decimal dots + Hz unit). These are
    // always on in the normal view; re-assert them here — not only in open() —
    // so they survive after an overlay or idle-blank cleared them.
    tmate2Seg(m_lcdVector, kSeg_DOT1, true);
    tmate2Seg(m_lcdVector, kSeg_DOT2, true);
    tmate2Seg(m_lcdVector, kSeg_HZ,   true);

    // Mode — clear all mode bits first, then set the matching one
    applyModeSegs(m_lcdVector, mode);

    // S-meter bargraph — set bars 1..N on, rest off
    const int bars = smeterBars(smeter_dbm);
    for (int i = 0; i < 15; ++i)
        tmate2Seg(m_lcdVector, kSMeterBars[i], i < bars);

    // RIT / XIT
    tmate2Seg(m_lcdVector, kSeg_RIT, rit);
    tmate2Seg(m_lcdVector, kSeg_XIT, xit);

    sendTMate2();
}

void HidEncoderManager::setTMate2OverlayIndicators(const QString& overlayType,
                                                    int overlayValue,
                                                    const QString& mode)
{
    if (!m_device || !isTMate2()) return;

    const bool isVolume = overlayType == QLatin1String("volume");
    const bool isPower  = overlayType == QLatin1String("power");
    const bool isSpeed  = overlayType == QLatin1String("speed");
    const bool isWpm    = overlayType == QLatin1String("wpm");
    const bool isRit    = overlayType == QLatin1String("rit");

    tmate2Seg(m_lcdVector, kSeg_SMETER_LINE, false);
    tmate2Seg(m_lcdVector, kSeg_DOT1, false);
    tmate2Seg(m_lcdVector, kSeg_DOT2, false);
    tmate2Seg(m_lcdVector, kSeg_VOL, isVolume);
    tmate2Seg(m_lcdVector, kSeg_W, isPower);
    tmate2Seg(m_lcdVector, kSeg_HZ, isSpeed || isRit);
    tmate2Seg(m_lcdVector, kSeg_RIT, isRit);
    tmate2Seg(m_lcdVector, kSeg_SMETER_DB_MINUS, isRit && overlayValue < 0);
    tmate2Seg(m_lcdVector, kSeg_DBM, false);
    tmate2Seg(m_lcdVector, kSeg_S, false);
    tmate2Seg(m_lcdVector, kSeg_RX, false);
    tmate2Seg(m_lcdVector, kSeg_TX, false);
    tmate2Seg(m_lcdVector, kSeg_XIT, false);

    // Keep the current mode visible; for WPM force CW as an additional cue.
    applyModeSegs(m_lcdVector, isWpm ? QStringLiteral("CW") : mode);

    const int bars = (isVolume || isPower)
        ? std::clamp((std::clamp(overlayValue, 0, 100) * 15 + 99) / 100, 0, 15)
        : 0;
    for (int i = 0; i < 15; ++i)
        tmate2Seg(m_lcdVector, kSMeterBars[i], i < bars);

    sendTMate2();
}

void HidEncoderManager::clearTMate2Indicators()
{
    if (!m_device || !isTMate2()) return;

    tmate2Seg(m_lcdVector, kSeg_RX, false);
    tmate2Seg(m_lcdVector, kSeg_TX, false);
    tmate2Seg(m_lcdVector, kSeg_S, false);
    tmate2Seg(m_lcdVector, kSeg_VOL, false);
    tmate2Seg(m_lcdVector, kSeg_SMETER_LINE, false);
    tmate2Seg(m_lcdVector, kSeg_SMETER_DB_MINUS, false);
    tmate2Seg(m_lcdVector, kSeg_DOT1, false);
    tmate2Seg(m_lcdVector, kSeg_DOT2, false);
    tmate2Seg(m_lcdVector, kSeg_HZ, false);
    tmate2Seg(m_lcdVector, kSeg_W, false);
    tmate2Seg(m_lcdVector, kSeg_RIT, false);
    tmate2Seg(m_lcdVector, kSeg_XIT, false);
    tmate2Seg(m_lcdVector, kSeg_DBM, false);
    applyModeSegs(m_lcdVector, QString());
    for (const auto& bar : kSMeterBars)
        tmate2Seg(m_lcdVector, bar, false);

    sendTMate2();
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
