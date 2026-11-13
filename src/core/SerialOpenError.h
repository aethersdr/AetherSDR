#pragma once

#include <QString>

namespace AetherSDR {

// Why a serial PTT/CW port failed to open, and what the operator can do
// about it (#6972). Qt reports EACCES, EBUSY and a held lock file all as
// QSerialPort::PermissionError, so the kind comes from probing the device
// node (stat/access/group), not from the Qt enum. The probe never opens the
// device: opening a tty raises DTR on most drivers, and DTR may be wired to
// PTT.
enum class SerialOpenFailureKind {
    None = 0,
    Permission,   // the device exists and this user may not read/write it
    NotFound,     // no such device (unplugged, renamed)
    InUse,        // accessible, but another program or lock holds it
    Other,
};

// What the probe saw of the device node. Filled by probeSerialDevice() on a
// real system; tests fill it directly (CI runs as root, where access() always
// succeeds).
struct SerialDeviceProbe {
    bool exists{false};
    int  statErrno{0};        // errno from stat() when !exists
    int  accessErrno{0};      // errno from access(R_OK|W_OK), 0 = allowed
    bool groupReadWrite{false};  // group has rw on the node
    bool inGroup{false};      // the process already carries the node's group
    QString groupName;        // the node's group (e.g. dialout, uucp)
};

struct SerialOpenFailure {
    SerialOpenFailureKind kind{SerialOpenFailureKind::None};
    QString device;     // full system path, e.g. /dev/ttyUSB0 or COM3
    QString error;      // the underlying error text, verbatim
    int nativeCode{0};  // errno or Win32 GetLastError(), 0 when unknown
    // Group whose membership would grant access. Set only when that is the
    // relevant remedy (Linux, group-rw node, process not in the group).
    QString group;
};

// Pure classification from the probe plus Qt's report. qtNotFound and
// qtPermission are QSerialPort::DeviceNotFoundError / PermissionError.
SerialOpenFailureKind classifySerialOpenFailure(const SerialDeviceProbe& probe,
                                                bool qtNotFound, bool qtPermission,
                                                int nativeCode);

// The group to recommend for a Permission failure, or empty when joining a
// group would not grant access (or the platform has no usermod/adduser).
QString serialGroupRemedy(const SerialDeviceProbe& probe);

// Win32 GetLastError() → kind. 5 (access denied) means another program has
// the exclusive COM handle far more often than an ACL.
SerialOpenFailureKind classifyWin32SerialOpenFailure(unsigned long win32Error);

#ifndef Q_OS_WIN
// Probe the device node. POSIX only; never opens the device.
SerialDeviceProbe probeSerialDevice(const QString& systemLocation);
#endif

// One-line summary: "<error>: Can't open <device> (errno N)".
QString serialOpenFailureSummary(const SerialOpenFailure& f);
// Remedy text for the dialog, plain text with the commands on their own lines
// so they can be selected and copied. Never empty for a failure.
QString serialOpenFailureAdvice(const SerialOpenFailure& f);

} // namespace AetherSDR
