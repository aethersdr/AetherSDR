#include <QtGlobal>
#ifdef Q_OS_MAC

#include "UlanziDialMacOSManager.h"
#include "core/LogManager.h"

#include <QDebug>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDValue.h>
#include <IOKit/hid/IOHIDElement.h>

// Linux-style keycode numeric values, mirrored so signatures stay
// consistent across all three platforms.
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

int hidConsumerToLinuxKey(int usage)
{
    switch (usage) {
        case 0xCD: return KEY_PLAYPAUSE;
        case 0xE2: return KEY_MUTE;
        case 0xB5: return KEY_NEXTSONG;
        case 0xB6: return KEY_PREVIOUSSONG;
        case 0xE9: return KEY_VOLUMEUP;
        case 0xEA: return KEY_VOLUMEDOWN;
        default:   return -1;
    }
}

int hidKbdToLinuxKey(int usage)
{
    switch (usage) {
        case 0x06: return KEY_C;
        case 0x19: return KEY_V;
        case 0x1C: return KEY_Y;
        case 0x1D: return KEY_Z;
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

// Build a matching dictionary so IOHIDManager only surfaces our dial,
// not every keyboard on the machine.  Match by product-name substring
// "Ulanzi Dial".
CFMutableDictionaryRef makeMatchDict()
{
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef product = CFSTR("Ulanzi Dial");
    CFDictionarySetValue(d, CFSTR(kIOHIDProductKey), product);
    return d;
}

} // namespace

UlanziDialMacOSManager::UlanziDialMacOSManager(QObject* parent)
    : QObject(parent) {}

UlanziDialMacOSManager::~UlanziDialMacOSManager() { stop(); }

void UlanziDialMacOSManager::start()
{
    if (m_manager) return;
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault,
                                             kIOHIDOptionsTypeNone);
    m_manager = mgr;

    CFMutableDictionaryRef match = makeMatchDict();
    IOHIDManagerSetDeviceMatching(mgr, match);
    CFRelease(match);

    IOHIDManagerRegisterDeviceMatchingCallback(mgr,
        reinterpret_cast<IOHIDDeviceCallback>(&UlanziDialMacOSManager::devMatchedCb), this);
    IOHIDManagerRegisterDeviceRemovalCallback(mgr,
        reinterpret_cast<IOHIDDeviceCallback>(&UlanziDialMacOSManager::devRemovedCb), this);
    IOHIDManagerRegisterInputValueCallback(mgr,
        reinterpret_cast<IOHIDValueCallback>(&UlanziDialMacOSManager::hidValueCb), this);

    IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetMain(), kCFRunLoopDefaultMode);

    // Open with seize-device so the dial's events stop reaching the
    // OS keyboard stack — the macOS equivalent of Linux EVIOCGRAB.
    IOHIDManagerOpen(mgr, kIOHIDOptionsTypeSeizeDevice);
}

void UlanziDialMacOSManager::stop()
{
    if (!m_manager) return;
    IOHIDManagerRef mgr = static_cast<IOHIDManagerRef>(m_manager);
    IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
    IOHIDManagerUnscheduleFromRunLoop(mgr, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    CFRelease(mgr);
    m_manager = nullptr;
    if (m_anyOpen) {
        const QString name = m_deviceName;
        m_deviceName.clear();
        m_anyOpen = false;
        emit connectionChanged(false, name);
    }
}

void UlanziDialMacOSManager::hidValueCb(void* ctx, int /*result*/, void* /*sender*/, void* valuePtr)
{
    auto* self = static_cast<UlanziDialMacOSManager*>(ctx);
    auto* value = static_cast<IOHIDValueRef>(valuePtr);
    IOHIDElementRef element = IOHIDValueGetElement(value);
    const int usagePage = IOHIDElementGetUsagePage(element);
    const int usage     = IOHIDElementGetUsage(element);
    const int v         = static_cast<int>(IOHIDValueGetIntegerValue(value));
    self->onHidValue(usagePage, usage, v);
}

void UlanziDialMacOSManager::devMatchedCb(void* ctx, int /*result*/, void* /*sender*/, void* devicePtr)
{
    auto* self = static_cast<UlanziDialMacOSManager*>(ctx);
    auto* device = static_cast<IOHIDDeviceRef>(devicePtr);
    CFStringRef nameRef = static_cast<CFStringRef>(
        IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductKey)));
    QString name;
    if (nameRef) {
        char buf[256] = {};
        CFStringGetCString(nameRef, buf, sizeof(buf) - 1, kCFStringEncodingUTF8);
        name = QString::fromUtf8(buf);
    }
    self->onDeviceMatching(name);
}

void UlanziDialMacOSManager::devRemovedCb(void* ctx, int /*result*/, void* /*sender*/, void* /*device*/)
{
    auto* self = static_cast<UlanziDialMacOSManager*>(ctx);
    self->onDeviceRemoval();
}

void UlanziDialMacOSManager::onDeviceMatching(const QString& productName)
{
    m_anyOpen = true;
    if (!productName.isEmpty()) m_deviceName = productName;
    qCInfo(lcDevices) << "UlanziDialMacOSManager: attached" << m_deviceName;
    emit connectionChanged(true, m_deviceName);
}

void UlanziDialMacOSManager::onDeviceRemoval()
{
    qCInfo(lcDevices) << "UlanziDialMacOSManager: detached";
    const QString name = m_deviceName;
    m_deviceName.clear();
    m_anyOpen = false;
    m_ctrlDown = false;
    m_lastNonModKey = -1;
    m_prevsongAlongsideCtrl = false;
    emit connectionChanged(false, name);
}

void UlanziDialMacOSManager::onHidValue(int usagePage, int usage, int value)
{
    // Consumer page = 0x0C, Keyboard/Keypad page = 0x07.
    int linuxKey = -1;
    if (usagePage == 0x0C)      linuxKey = hidConsumerToLinuxKey(usage);
    else if (usagePage == 0x07) linuxKey = hidKbdToLinuxKey(usage);
    if (linuxKey < 0) return;
    emitKeyTransition(linuxKey, value ? 1 : 0);
}

void UlanziDialMacOSManager::emitKeyTransition(int linuxKey, int value)
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
        return;
    }
    if (!m_ctrlDown) {
        if (value == 1 || value == 0)
            emit buttonEvent(bareKeySignature(linuxKey), value);
        return;
    }
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

#endif // Q_OS_MAC
