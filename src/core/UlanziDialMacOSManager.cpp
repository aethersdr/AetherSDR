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
#include <IOKit/hid/IOHIDProperties.h>
#include <IOKit/hidsystem/IOHIDEventSystemClient.h>
#include <IOKit/hidsystem/IOHIDParameter.h>
#include <IOKit/hidsystem/IOHIDServiceClient.h>

#include <vector>

namespace AetherSDR {

namespace {

// Reporter's Bluetooth LE descriptor dump in #5126 records VendorID 65521
// (0xFFF1) and ProductID 130 (0x0082); the bench device reports the same pair.
constexpr int kUlanziVendorId = 0xFFF1;
constexpr int kUlanziProductId = 0x0082;

struct HidUsage {
    uint32_t page;
    uint32_t usage;
};

constexpr HidUsage kSuppressedUsages[] = {
    {0x0C, 0xCD}, {0x0C, 0xE2}, {0x0C, 0xB5}, {0x0C, 0xB6},
    {0x0C, 0xE9}, {0x0C, 0xEA}, {0x07, 0x06}, {0x07, 0x19},
    {0x07, 0x1C}, {0x07, 0x1D}, {0x07, 0xE0}, {0x07, 0xE4},
};

quint64 usageCode(uint32_t page, uint32_t usage)
{
    return (static_cast<quint64>(page) << 32) | usage;
}

bool isSuppressedUsage(quint64 code)
{
    for (const HidUsage& usage : kSuppressedUsages) {
        if (code == usageCode(usage.page, usage.usage)) {
            return true;
        }
    }
    return false;
}

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

qint64 serviceNumber(IOHIDServiceClientRef service, CFStringRef key)
{
    CFTypeRef property = IOHIDServiceClientCopyProperty(service, key);
    if (!property || CFGetTypeID(property) != CFNumberGetTypeID()) {
        if (property) {
            CFRelease(property);
        }
        return -1;
    }
    qint64 value = -1;
    CFNumberGetValue(static_cast<CFNumberRef>(property), kCFNumberSInt64Type, &value);
    CFRelease(property);
    return value;
}

CFMutableArrayRef makeSuppressionMapping(CFTypeRef previous)
{
    if (previous && CFGetTypeID(previous) != CFArrayGetTypeID()) {
        return nullptr;
    }

    CFMutableArrayRef mapping = CFArrayCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    if (previous) {
        const CFArrayRef previousArray = static_cast<CFArrayRef>(previous);
        const CFIndex count = CFArrayGetCount(previousArray);
        for (CFIndex index = 0; index < count; ++index) {
            const CFTypeRef entry = static_cast<CFTypeRef>(CFArrayGetValueAtIndex(previousArray, index));
            bool replace = false;
            if (CFGetTypeID(entry) == CFDictionaryGetTypeID()) {
                const CFTypeRef source = static_cast<CFTypeRef>(CFDictionaryGetValue(
                    static_cast<CFDictionaryRef>(entry), CFSTR(kIOHIDKeyboardModifierMappingSrcKey)));
                if (source && CFGetTypeID(source) == CFNumberGetTypeID()) {
                    quint64 sourceCode = 0;
                    CFNumberGetValue(static_cast<CFNumberRef>(source),
                                     kCFNumberSInt64Type, &sourceCode);
                    replace = isSuppressedUsage(sourceCode);
                }
            }
            if (!replace) {
                CFArrayAppendValue(mapping, entry);
            }
        }
    }

    for (const HidUsage& usage : kSuppressedUsages) {
        const quint64 sourceCode = usageCode(usage.page, usage.usage);
        // Keyboard usage 0 is reserved and produces no system event. The raw
        // IOHIDManager value callback still receives the original usage.
        const quint64 destinationCode = usageCode(0x07, 0x00);
        CFNumberRef source = CFNumberCreate(
            kCFAllocatorDefault, kCFNumberSInt64Type, &sourceCode);
        CFNumberRef destination = CFNumberCreate(
            kCFAllocatorDefault, kCFNumberSInt64Type, &destinationCode);
        CFMutableDictionaryRef entry = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(entry, CFSTR(kIOHIDKeyboardModifierMappingSrcKey), source);
        CFDictionarySetValue(entry, CFSTR(kIOHIDKeyboardModifierMappingDstKey), destination);
        CFArrayAppendValue(mapping, entry);
        CFRelease(entry);
        CFRelease(destination);
        CFRelease(source);
    }
    return mapping;
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

UlanziDialMacOSManager::~UlanziDialMacOSManager()
{
    stop();
    // A failed property write retains the service and original mapping so
    // teardown can retry. If all attempts fail, tell the operator exactly how
    // to recover; the event-system mapping otherwise lasts until reconnect.
    for (int attempt = 0; attempt < 2 && m_suppressedService; ++attempt) {
        restoreSystemEventSuppression();
    }
    if (m_suppressedService) {
        qCWarning(lcDevices)
            << "UlanziDialMacOSManager: prior key mapping could not be restored;"
               " reconnect the Ulanzi Dial to clear the temporary suppression";
    }
    discardSystemEventSuppression();
}

void UlanziDialMacOSManager::start()
{
    if (m_manager) return;
    if (m_suppressedService) {
        restoreSystemEventSuppression();
        if (m_suppressedService) {
            qCWarning(lcDevices)
                << "UlanziDialMacOSManager: cannot restart before restoring the prior key mapping";
            return;
        }
    }
    m_accessMode = AccessMode::None;
    m_openAttempted = false;
    m_lastOpenResult = 0;
    m_exclusiveOpenResult = 0;
    m_sharedOpenAttempted = false;
    m_sharedOpenResult = 0;
    m_systemEventsSuppressed = false;
    m_previousMappingPreserved = false;
    m_suppressionStatus = QStringLiteral("notNeeded");
    m_restorationStatus = QStringLiteral("notNeeded");

    const auto createManager = [this]() -> IOHIDManagerRef {
        IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault,
                                                     kIOHIDOptionsTypeNone);
        if (!manager) {
            qCWarning(lcDevices) << "UlanziDialMacOSManager: failed to create HID manager";
            return nullptr;
        }

        CFMutableDictionaryRef match = makeMatchDict();
        IOHIDManagerSetDeviceMatching(manager, match);
        CFRelease(match);

        IOHIDManagerRegisterDeviceMatchingCallback(manager,
            reinterpret_cast<IOHIDDeviceCallback>(&UlanziDialMacOSManager::devMatchedCb), this);
        IOHIDManagerRegisterDeviceRemovalCallback(manager,
            reinterpret_cast<IOHIDDeviceCallback>(&UlanziDialMacOSManager::devRemovedCb), this);
        IOHIDManagerRegisterInputValueCallback(manager,
            reinterpret_cast<IOHIDValueCallback>(&UlanziDialMacOSManager::hidValueCb), this);
        IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
        return manager;
    };

    IOHIDManagerRef mgr = createManager();
    if (!mgr) {
        return;
    }

    // Seize only the numerically matched dial so its events stop reaching the
    // OS keyboard stack — the macOS equivalent of Linux EVIOCGRAB.
    const IOReturn result = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeSeizeDevice);
    m_openAttempted = true;
    m_exclusiveOpenResult = static_cast<qint32>(result);
    m_lastOpenResult = static_cast<qint32>(result);
    if (result == kIOReturnSuccess) {
        m_manager = mgr;
        m_accessMode = AccessMode::Exclusive;
        return;
    }

    IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
    IOHIDManagerUnscheduleFromRunLoop(mgr, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    CFRelease(mgr);

    if (result != kIOReturnNotPrivileged) {
        qCWarning(lcDevices)
            << "UlanziDialMacOSManager: failed to exclusively open HID manager" << result;
        return;
    }

    qCWarning(lcDevices)
        << "UlanziDialMacOSManager: exclusive HID access denied; opening shared";
    mgr = createManager();
    if (!mgr) {
        return;
    }

    const IOReturn sharedResult = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
    m_sharedOpenAttempted = true;
    m_sharedOpenResult = static_cast<qint32>(sharedResult);
    m_lastOpenResult = static_cast<qint32>(sharedResult);
    if (sharedResult != kIOReturnSuccess) {
        qCWarning(lcDevices)
            << "UlanziDialMacOSManager: failed to open HID manager in shared mode" << sharedResult;
        IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
        IOHIDManagerUnscheduleFromRunLoop(mgr, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
        CFRelease(mgr);
        return;
    }

    m_manager = mgr;
    m_accessMode = AccessMode::Shared;
    if (m_anyOpen) {
        applySystemEventSuppression();
    }
}

QString UlanziDialMacOSManager::accessModeName() const
{
    switch (m_accessMode) {
        case AccessMode::Exclusive:
            return QStringLiteral("exclusive");
        case AccessMode::Shared:
            return QStringLiteral("shared");
        case AccessMode::None:
            return QStringLiteral("none");
    }
    return QStringLiteral("none");
}

void UlanziDialMacOSManager::applySystemEventSuppression()
{
    if (m_accessMode != AccessMode::Shared || m_systemEventsSuppressed) {
        return;
    }

    m_suppressionStatus = QStringLiteral("serviceUnavailable");
    m_restorationStatus = QStringLiteral("notAttempted");

    IOHIDEventSystemClientRef client =
        IOHIDEventSystemClientCreateSimpleClient(kCFAllocatorDefault);
    if (!client) {
        qCWarning(lcDevices)
            << "UlanziDialMacOSManager: failed to create HID event-system client";
        return;
    }

    CFArrayRef services = IOHIDEventSystemClientCopyServices(client);
    IOHIDServiceClientRef matchedService = nullptr;
    int matchedCount = 0;
    if (services) {
        const CFIndex count = CFArrayGetCount(services);
        for (CFIndex index = 0; index < count; ++index) {
            IOHIDServiceClientRef service = static_cast<IOHIDServiceClientRef>(
                const_cast<void*>(CFArrayGetValueAtIndex(services, index)));
            if (serviceNumber(service, CFSTR(kIOHIDVendorIDKey)) == kUlanziVendorId
                && serviceNumber(service, CFSTR(kIOHIDProductIDKey)) == kUlanziProductId) {
                matchedService = service;
                ++matchedCount;
            }
        }
    }

    if (matchedCount != 1 || !matchedService) {
        m_suppressionStatus = matchedCount > 1
            ? QStringLiteral("ambiguousServices")
            : QStringLiteral("serviceUnavailable");
        qCWarning(lcDevices)
            << "UlanziDialMacOSManager: expected one Ulanzi HID event service, found"
            << matchedCount;
        if (services) {
            CFRelease(services);
        }
        CFRelease(client);
        return;
    }

    CFTypeRef previous = IOHIDServiceClientCopyProperty(
        matchedService, CFSTR(kIOHIDUserKeyUsageMapKey));
    CFMutableArrayRef mapping = makeSuppressionMapping(previous);
    if (!mapping) {
        m_suppressionStatus = QStringLiteral("unsupportedPreviousMapping");
        qCWarning(lcDevices)
            << "UlanziDialMacOSManager: existing Ulanzi key mapping has an unsupported type";
        if (previous) {
            CFRelease(previous);
        }
        CFRelease(services);
        CFRelease(client);
        return;
    }

    // IOHIDServiceClient may return a cached property-list object whose
    // contents change when the service property is replaced. Preserve an
    // immutable value snapshot, not the live object, for later restoration.
    CFPropertyListRef previousSnapshot = nullptr;
    if (previous) {
        previousSnapshot = CFPropertyListCreateDeepCopy(
            kCFAllocatorDefault, previous, kCFPropertyListImmutable);
        CFRelease(previous);
        previous = nullptr;
        if (!previousSnapshot) {
            CFRelease(mapping);
            m_suppressionStatus = QStringLiteral("snapshotFailed");
            qCWarning(lcDevices)
                << "UlanziDialMacOSManager: failed to snapshot the prior key mapping";
            CFRelease(services);
            CFRelease(client);
            return;
        }
    }

    const bool applied = IOHIDServiceClientSetProperty(
        matchedService, CFSTR(kIOHIDUserKeyUsageMapKey), mapping);
    CFRelease(mapping);
    if (!applied) {
        m_suppressionStatus = QStringLiteral("setFailed");
        qCWarning(lcDevices)
            << "UlanziDialMacOSManager: failed to suppress Ulanzi system key events";
        if (previousSnapshot) {
            CFRelease(previousSnapshot);
        }
        CFRelease(services);
        CFRelease(client);
        return;
    }

    m_suppressedService = matchedService;
    CFRetain(static_cast<CFTypeRef>(m_suppressedService));
    // IOHIDServiceClient is owned by its event-system client. Retaining only
    // the service leaves its internal client lock dangling and crashes a later
    // restore write. Keep the create reference until restoration/disconnect.
    m_eventSystemClient = client;
    m_previousUserKeyMapping = const_cast<void*>(previousSnapshot);
    m_previousMappingPreserved = true;
    m_systemEventsSuppressed = true;
    m_suppressionStatus = QStringLiteral("active");

    CFRelease(services);
}

void UlanziDialMacOSManager::restoreSystemEventSuppression()
{
    if (!m_suppressedService) {
        return;
    }

    CFArrayRef emptyMapping = nullptr;
    CFTypeRef restoreValue = static_cast<CFTypeRef>(m_previousUserKeyMapping);
    if (!restoreValue) {
        emptyMapping = CFArrayCreate(
            kCFAllocatorDefault, nullptr, 0, &kCFTypeArrayCallBacks);
        restoreValue = emptyMapping;
    }

    const bool restored = IOHIDServiceClientSetProperty(
        static_cast<IOHIDServiceClientRef>(m_suppressedService),
        CFSTR(kIOHIDUserKeyUsageMapKey), restoreValue);
    if (emptyMapping) {
        CFRelease(emptyMapping);
    }
    if (!restored) {
        qCWarning(lcDevices)
            << "UlanziDialMacOSManager: failed to restore the prior Ulanzi key mapping";
    }
    m_restorationStatus = restored ? QStringLiteral("success")
                                   : QStringLiteral("failed");
    m_systemEventsSuppressed = !restored;
    m_suppressionStatus = restored ? QStringLiteral("restored")
                                   : QStringLiteral("restoreFailed");
    if (!restored) {
        return;
    }

    if (m_previousUserKeyMapping) {
        CFRelease(static_cast<CFTypeRef>(m_previousUserKeyMapping));
        m_previousUserKeyMapping = nullptr;
    }
    CFRelease(static_cast<CFTypeRef>(m_suppressedService));
    m_suppressedService = nullptr;
    CFRelease(static_cast<CFTypeRef>(m_eventSystemClient));
    m_eventSystemClient = nullptr;
    m_previousMappingPreserved = false;
}

void UlanziDialMacOSManager::discardSystemEventSuppression()
{
    if (m_previousUserKeyMapping) {
        CFRelease(static_cast<CFTypeRef>(m_previousUserKeyMapping));
        m_previousUserKeyMapping = nullptr;
    }
    if (m_suppressedService) {
        CFRelease(static_cast<CFTypeRef>(m_suppressedService));
        m_suppressedService = nullptr;
    }
    if (m_eventSystemClient) {
        CFRelease(static_cast<CFTypeRef>(m_eventSystemClient));
        m_eventSystemClient = nullptr;
    }
    m_previousMappingPreserved = false;
    m_systemEventsSuppressed = false;
    m_suppressionStatus = QStringLiteral("deviceRemoved");
    m_restorationStatus = QStringLiteral("notNeeded");
}

QJsonObject UlanziDialMacOSManager::diagnostics() const
{
    QJsonArray devices;

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
            devices.append(QJsonObject{
                {QStringLiteral("product"), deviceString(device, CFSTR(kIOHIDProductKey))},
                {QStringLiteral("vendorId"), vendorId},
                {QStringLiteral("productId"), productId},
                {QStringLiteral("primaryUsagePage"),
                 deviceNumber(device, CFSTR(kIOHIDPrimaryUsagePageKey))},
                {QStringLiteral("primaryUsage"),
                 deviceNumber(device, CFSTR(kIOHIDPrimaryUsageKey))},
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
        {QStringLiteral("productionMatch"),
         QJsonObject{{QStringLiteral("vendorId"), kUlanziVendorId},
                     {QStringLiteral("productId"), kUlanziProductId}}},
        {QStringLiteral("matchedCount"), devices.size()},
        {QStringLiteral("matchedDevices"), devices},
        {QStringLiteral("inventoryAvailable"), inventoryAvailable},
        {QStringLiteral("accessMode"), accessModeName()},
        {QStringLiteral("exclusiveClaimActive"), m_accessMode == AccessMode::Exclusive},
        {QStringLiteral("connected"), m_anyOpen},
        {QStringLiteral("deviceName"), m_deviceName},
        {QStringLiteral("openAttempted"), m_openAttempted},
        {QStringLiteral("lastOpenResult"), m_lastOpenResult},
        {QStringLiteral("lastOpenStatus"),
         openResultName(m_openAttempted, m_lastOpenResult)},
        {QStringLiteral("exclusiveOpenResult"), m_exclusiveOpenResult},
        {QStringLiteral("exclusiveOpenStatus"),
         openResultName(m_openAttempted, m_exclusiveOpenResult)},
        {QStringLiteral("sharedOpenAttempted"), m_sharedOpenAttempted},
        {QStringLiteral("sharedOpenResult"), m_sharedOpenResult},
        {QStringLiteral("sharedOpenStatus"),
         openResultName(m_sharedOpenAttempted, m_sharedOpenResult)},
        {QStringLiteral("systemEventsSuppressed"), m_systemEventsSuppressed},
        {QStringLiteral("suppressionStatus"), m_suppressionStatus},
        {QStringLiteral("previousMappingPreserved"), m_previousMappingPreserved},
        {QStringLiteral("restorationStatus"), m_restorationStatus},
        {QStringLiteral("eventSystemClientRetained"), m_eventSystemClient != nullptr},
    };
}

void UlanziDialMacOSManager::stop()
{
    restoreSystemEventSuppression();
    if (!m_manager) return;
    IOHIDManagerRef mgr = static_cast<IOHIDManagerRef>(m_manager);
    IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
    IOHIDManagerUnscheduleFromRunLoop(mgr, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    CFRelease(mgr);
    m_manager = nullptr;
    m_accessMode = AccessMode::None;
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
    applySystemEventSuppression();
}

void UlanziDialMacOSManager::onDeviceRemoval()
{
    // The mapping belongs to the removed service and vanishes with it. Do not
    // try to restore through a stale service; a reconnect gets fresh state.
    discardSystemEventSuppression();
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
