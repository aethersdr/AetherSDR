#include "Ctr2HidapiPort.h"

#include "Ctr2HidThreadPort.h"

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

// hid_init() is idempotent. hid_exit() is never called: it is process-wide
// and other HID controllers may still be open.
bool ensureHidInit()
{
    return hid_init() == 0;
}

} // namespace

QList<Ctr2HidPort::DeviceInfo> Ctr2HidapiPort::enumerate()
{
    QList<Ctr2HidPort::DeviceInfo> out;
    if (!ensureHidInit()) {
        return out;
    }
    hid_device_info* list = hid_enumerate(0, 0);
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
    std::stable_sort(out.begin(), out.end(),
                     [](const Ctr2HidPort::DeviceInfo& a, const Ctr2HidPort::DeviceInfo& b) {
        return !a.ctr2Model().isEmpty() && b.ctr2Model().isEmpty();
    });
    return out;
}

Ctr2HidPort* Ctr2HidapiPort::open(const Ctr2HidPort::DeviceInfo& device, QString* error,
                                  QObject* parent)
{
    if (!ensureHidInit()) {
        *error = QStringLiteral("USB HID support failed to initialize");
        return nullptr;
    }
    errno = 0;
    hid_device* handle = hid_open_path(device.path.toUtf8().constData());
    if (!handle) {
        const int openErrno = errno;
        const QString why = fromWide(hid_error(nullptr));
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
    io.close = [handle] { hid_close(handle); };
    io.lastError = [handle] { return fromWide(hid_error(handle)); };
    return new Ctr2HidThreadPort(std::move(io), device.label(), device.path, parent);
}

} // namespace AetherSDR
