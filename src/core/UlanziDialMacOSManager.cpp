#include <QtGlobal>
#ifdef Q_OS_MAC

#include "UlanziDialMacOSManager.h"
#include "core/LogManager.h"
#include "core/UlanziChordDecoder.h"

#include <QDebug>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDValue.h>
#include <IOKit/hid/IOHIDElement.h>

namespace AetherSDR {

namespace {

int hidConsumerToLinuxKey(int usage)
{
    switch (usage) {
        case 0xCD: return UlanziKey::PlayPause;
        case 0xE2: return UlanziKey::Mute;
        case 0xB5: return UlanziKey::NextSong;
        case 0xB6: return UlanziKey::PreviousSong;
        case 0xE9: return UlanziKey::VolumeUp;
        case 0xEA: return UlanziKey::VolumeDown;
        default:   return -1;
    }
}

int hidKbdToLinuxKey(int usage)
{
    switch (usage) {
        case 0x06: return UlanziKey::C;
        case 0x19: return UlanziKey::V;
        case 0x1C: return UlanziKey::Y;
        case 0x1D: return UlanziKey::Z;
        case 0xE0: return UlanziKey::LeftCtrl;
        case 0xE4: return UlanziKey::RightCtrl;
        default:   return -1;
    }
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
    m_decoder.reset();
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
    for (const auto& out : m_decoder.feed(linuxKey, value)) {
        if (out.kind == UlanziChordDecoder::Event::Kind::Tune)
            emit tuneSteps(out.tuneSteps);
        else
            emit buttonEvent(out.signature, out.action);
    }
}

} // namespace AetherSDR

#endif // Q_OS_MAC
