#include <QtGlobal>
#ifdef Q_OS_MAC

#include "UlanziDialMacOSManager.h"
#include "core/LogManager.h"
#include "core/UlanziChordDecoder.h"

#include <QDebug>
#include <QJsonArray>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDValue.h>
#include <IOKit/hid/IOHIDElement.h>

#include <vector>

namespace AetherSDR {

namespace {

constexpr int kUlanziVendorId = 0xFFF1;
constexpr int kUlanziProductId = 0x0082;

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

// Build a matching dictionary so IOHIDManager only surfaces the Ulanzi Dial,
// not unrelated Apple HID devices. Product-only matching was not sufficiently
// specific for an exclusive claim on macOS 26 (#5126), so use the dial's
// observed numeric vendor and product identifiers.
CFMutableDictionaryRef makeMatchDict()
{
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFNumberRef vendor =
        CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &kUlanziVendorId);
    CFNumberRef product =
        CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &kUlanziProductId);
    CFDictionarySetValue(d, CFSTR(kIOHIDVendorIDKey), vendor);
    CFDictionarySetValue(d, CFSTR(kIOHIDProductIDKey), product);
    CFRelease(vendor);
    CFRelease(product);
    return d;
}

qint64 deviceNumber(IOHIDDeviceRef device, CFStringRef key)
{
    CFTypeRef property = IOHIDDeviceGetProperty(device, key);
    if (!property || CFGetTypeID(property) != CFNumberGetTypeID()) {
        return -1;
    }
    qint64 value = -1;
    CFNumberGetValue(static_cast<CFNumberRef>(property), kCFNumberSInt64Type, &value);
    return value;
}

QString deviceString(IOHIDDeviceRef device, CFStringRef key)
{
    CFTypeRef property = IOHIDDeviceGetProperty(device, key);
    if (!property || CFGetTypeID(property) != CFStringGetTypeID()) {
        return {};
    }
    const CFStringRef text = static_cast<CFStringRef>(property);
    const CFIndex length = CFStringGetLength(text);
    const CFIndex bytes =
        CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    QByteArray utf8(static_cast<qsizetype>(bytes), '\0');
    if (!CFStringGetCString(text, utf8.data(), bytes, kCFStringEncodingUTF8)) {
        return {};
    }
    return QString::fromUtf8(utf8.constData());
}

QString openResultName(bool attempted, qint32 result)
{
    if (!attempted) {
        return QStringLiteral("notAttempted");
    }
    if (result == kIOReturnSuccess) {
        return QStringLiteral("success");
    }
    if (result == kIOReturnNotPrivileged) {
        return QStringLiteral("notPrivileged");
    }
    if (result == kIOReturnExclusiveAccess) {
        return QStringLiteral("exclusiveAccess");
    }
    return QStringLiteral("error");
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
    if (!mgr) {
        qCWarning(lcDevices) << "UlanziDialMacOSManager: failed to create HID manager";
        return;
    }

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

    // Seize only the numerically matched dial so its events stop reaching the
    // OS keyboard stack — the macOS equivalent of Linux EVIOCGRAB.
    const IOReturn result = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeSeizeDevice);
    m_openAttempted = true;
    m_lastOpenResult = static_cast<qint32>(result);
    if (result != kIOReturnSuccess) {
        qCWarning(lcDevices)
            << "UlanziDialMacOSManager: failed to open HID manager" << result;
        IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
        IOHIDManagerUnscheduleFromRunLoop(mgr, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
        CFRelease(mgr);
        return;
    }
    m_manager = mgr;
}

QJsonObject UlanziDialMacOSManager::diagnostics() const
{
    QJsonArray devices;
    int expectedMatchCount = 0;
    int unexpectedMatchCount = 0;

    IOHIDManagerRef probe = IOHIDManagerCreate(kCFAllocatorDefault,
                                               kIOHIDOptionsTypeNone);
    const bool inventoryAvailable = probe != nullptr;
    if (probe) {
        CFMutableDictionaryRef match = makeMatchDict();
        IOHIDManagerSetDeviceMatching(probe, match);
        CFRelease(match);

        CFSetRef matched = IOHIDManagerCopyDevices(probe);
        const CFIndex count = matched ? CFSetGetCount(matched) : 0;
        std::vector<const void*> values(static_cast<std::size_t>(count));
        if (matched) {
            CFSetGetValues(matched, values.data());
        }
        for (const void* value : values) {
            IOHIDDeviceRef device = static_cast<IOHIDDeviceRef>(const_cast<void*>(value));
            const qint64 vendorId = deviceNumber(device, CFSTR(kIOHIDVendorIDKey));
            const qint64 productId = deviceNumber(device, CFSTR(kIOHIDProductIDKey));
            const bool expected = vendorId == kUlanziVendorId
                && productId == kUlanziProductId;
            if (expected) {
                ++expectedMatchCount;
            } else {
                ++unexpectedMatchCount;
            }
            devices.append(QJsonObject{
                {QStringLiteral("product"), deviceString(device, CFSTR(kIOHIDProductKey))},
                {QStringLiteral("vendorId"), vendorId},
                {QStringLiteral("productId"), productId},
                {QStringLiteral("primaryUsagePage"),
                 deviceNumber(device, CFSTR(kIOHIDPrimaryUsagePageKey))},
                {QStringLiteral("primaryUsage"),
                 deviceNumber(device, CFSTR(kIOHIDPrimaryUsageKey))},
                {QStringLiteral("expected"), expected},
            });
        }
        if (matched) {
            CFRelease(matched);
        }
        CFRelease(probe);
    }

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("diagnostic"), QStringLiteral("ulanzi")},
        {QStringLiteral("platform"), QStringLiteral("macos")},
        {QStringLiteral("supported"), true},
        {QStringLiteral("expectedMatch"),
         QJsonObject{{QStringLiteral("vendorId"), kUlanziVendorId},
                     {QStringLiteral("productId"), kUlanziProductId}}},
        {QStringLiteral("matchedCount"), devices.size()},
        {QStringLiteral("expectedMatchCount"), expectedMatchCount},
        {QStringLiteral("unexpectedMatchCount"), unexpectedMatchCount},
        {QStringLiteral("matchScopeSafe"), unexpectedMatchCount == 0},
        {QStringLiteral("matchedDevices"), devices},
        {QStringLiteral("inventoryAvailable"), inventoryAvailable},
        {QStringLiteral("exclusiveClaimActive"), m_manager != nullptr},
        {QStringLiteral("connected"), m_anyOpen},
        {QStringLiteral("deviceName"), m_deviceName},
        {QStringLiteral("openAttempted"), m_openAttempted},
        {QStringLiteral("lastOpenResult"), m_lastOpenResult},
        {QStringLiteral("lastOpenStatus"),
         openResultName(m_openAttempted, m_lastOpenResult)},
    };
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
