#include "Ctr2HidapiPort.h"

#include "Ctr2DeviceLock.h"
#include "Ctr2HidThreadPort.h"
#include "HidDiscovery.h"

#include <hidapi/hidapi.h>

#include <algorithm>
#include <cerrno>
#include <utility>

namespace AetherSDR {

namespace {

constexpr unsigned short kUsagePage = 0xFF00;
constexpr unsigned short kUsage = 0x01;

QString fromWide(const wchar_t* s)
{
    return s ? QString::fromWCharArray(s) : QString();
}

bool ensureHidInit()
{
    return HidDiscovery::init() == 0;
}

} // namespace

QList<Ctr2HidPort::DeviceInfo> Ctr2HidapiPort::enumerate()
{
    QList<Ctr2HidPort::DeviceInfo> out;
    if (!ensureHidInit()) {
        return out;
    }
    // Ask only for the known VID:PID pairs: hid_enumerate(0, 0) makes the
    // Windows backend request strings and descriptors from every HID device
    // on the machine, a GUI-thread stall on each rescan (#6256 has the detail).
    QList<std::pair<quint16, quint16>> ids;
    for (const Ctr2HidPort::KnownCtr2& known : Ctr2HidPort::knownCtr2Devices()) {
        const std::pair<quint16, quint16> id{known.vendorId, known.productId};
        if (!ids.contains(id)) {
            ids.append(id);
        }
    }
    for (const auto& [vendorId, productId] : std::as_const(ids)) {
        hid_device_info* list = HidDiscovery::enumerate(vendorId, productId);
        for (hid_device_info* d = list; d; d = d->next) {
            if (d->usage_page != kUsagePage || d->usage != kUsage) {
                continue;
            }
            Ctr2HidPort::DeviceInfo info;
            info.path = QString::fromUtf8(d->path);
            info.vendorId = d->vendor_id;
            info.productId = d->product_id;
            info.manufacturer = fromWide(d->manufacturer_string);
            info.product = fromWide(d->product_string);
            info.serial = fromWide(d->serial_number);
            out.append(info);
        }
        hid_free_enumeration(list);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Ctr2HidPort::DeviceInfo& a, const Ctr2HidPort::DeviceInfo& b) {
        return !a.ctr2Model().isEmpty() && b.ctr2Model().isEmpty();
    });
    return out;
}

Ctr2HidPort* Ctr2HidapiPort::open(const Ctr2HidPort::DeviceInfo& device, QString* error,
                                  QObject* parent, quint32 hostCapabilities)
{
    if (!ensureHidInit()) {
        *error = QStringLiteral("USB HID support failed to initialize");
        return nullptr;
    }
    // One AetherSDR at a time on a device: two relays on one CTR2 break
    // each other's link.
    qint64 otherPid = 0;
    Ctr2DeviceLock::Holder lock = Ctr2DeviceLock::acquire(device.path, &otherPid);
    if (!lock) {
        *error = otherPid > 0 ? QStringLiteral("%1 is in use by another AetherSDR (process %2). "
                                               "Stop its CTR2 relay, or close it.")
                                    .arg(device.label())
                                    .arg(otherPid)
                              : QStringLiteral("%1 is in use by another AetherSDR. Stop its CTR2 "
                                               "relay, or close it.")
                                    .arg(device.label());
        return nullptr;
    }
    errno = 0;
    std::wstring openError;
    hid_device* handle = HidDiscovery::openPath(device.path.toUtf8().constData(), &openError);
    if (!handle) {
        const int openErrno = errno;
        const QString why = fromWide(openError.c_str());
        *error = QStringLiteral("Cannot open %1: %2")
            .arg(device.label(), why.isEmpty() ? QStringLiteral("HID I/O error") : why);
#ifdef Q_OS_LINUX
        // hidraw nodes are root-only without a udev rule; name the fix.
        if (openErrno == EACCES) {
            *error += QStringLiteral(". Press Start to install the CTR2 access rule.");
        }
#else
        Q_UNUSED(openErrno);
#endif
        return nullptr;
    }
    hid_set_nonblocking(handle, 1);

    Ctr2HidDeviceIo io;
    io.read = [handle](unsigned char* buffer, int size) {
        return hid_read(handle, buffer, static_cast<size_t>(size));
    };
    io.write = [handle](const unsigned char* buffer, int size) {
        return hid_write(handle, buffer, static_cast<size_t>(size));
    };
    io.getFeature = [handle](unsigned char* buffer, int size) {
        return hid_get_feature_report(handle, buffer, static_cast<size_t>(size));
    };
    io.setFeature = [handle](const unsigned char* buffer, int size) {
        return hid_send_feature_report(handle, buffer, static_cast<size_t>(size));
    };
    // The device is released for other instances once it is closed.
    auto held = std::make_shared<Ctr2DeviceLock::Holder>(std::move(lock));
    io.close = [handle, held] {
        hid_close(handle);
        held->reset();
    };
    io.lastError = [handle] { return fromWide(hid_error(handle)); };
    return new Ctr2HidThreadPort(std::move(io), device.label(), device.path, parent,
                                 hostCapabilities);
}

} // namespace AetherSDR
