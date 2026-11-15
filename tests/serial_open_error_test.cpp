// Serial PTT/CW open-failure classification and advice (#6972). The probe is
// injected: CI runs as root, where access() always succeeds, so the permission
// case can only be exercised through the pure classifier.

#include "core/SerialOpenError.h"

#include <cerrno>
#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "serial_open_error_test: FAIL: %s\n", what);
        ++g_failures;
    }
}

SerialDeviceProbe deniedDialoutNode()
{
    SerialDeviceProbe p;
    p.exists = true;
    p.accessErrno = EACCES;
    p.groupReadWrite = true;    // crw-rw---- root:dialout
    p.inGroup = false;
    p.groupName = QStringLiteral("dialout");
    return p;
}

void testPermissionWithGroupRemedy()
{
    const SerialDeviceProbe p = deniedDialoutNode();
    check(classifySerialOpenFailure(p, false, true, 0) == SerialOpenFailureKind::Permission,
          "EACCES on an existing node is a permission failure");
    check(serialGroupRemedy(p) == QStringLiteral("dialout"),
          "the node's group is the remedy");

    SerialDeviceProbe uucp = p;
    uucp.groupName = QStringLiteral("uucp");
    check(serialGroupRemedy(uucp) == QStringLiteral("uucp"),
          "the group comes from the node, not a hardcoded dialout");

    SerialOpenFailure f;
    f.kind = SerialOpenFailureKind::Permission;
    f.device = QStringLiteral("/dev/ttyUSB0");
    f.error = QStringLiteral("Permission denied");
    f.nativeCode = EACCES;
    f.group = QStringLiteral("dialout");
    const QString summary = serialOpenFailureSummary(f);
    check(summary.startsWith(QStringLiteral("Permission denied: Can't open /dev/ttyUSB0")),
          "summary keeps the full error and the full device path");
    check(summary.contains(QStringLiteral("%1").arg(EACCES)), "summary carries the errno");
    const QString advice = serialOpenFailureAdvice(f);
    check(advice.contains(QStringLiteral("sudo usermod -aG dialout \"$USER\"")),
          "usermod command for the named group");
    check(advice.contains(QStringLiteral("sudo adduser \"$USER\" dialout")),
          "Debian/Ubuntu adduser alternative");
    check(advice.contains(QStringLiteral("log out")), "logout/login instruction");
}

void testNoGroupAdviceWhenItWouldNotHelp()
{
    SerialDeviceProbe inGroup = deniedDialoutNode();
    inGroup.inGroup = true;   // already a member, still denied: ACL/MAC
    check(serialGroupRemedy(inGroup).isEmpty(), "member of the group: no group advice");

    SerialDeviceProbe noGroupRw = deniedDialoutNode();
    noGroupRw.groupReadWrite = false;   // crw------- root:root
    check(serialGroupRemedy(noGroupRw).isEmpty(), "group cannot rw: no group advice");

    SerialDeviceProbe tty = deniedDialoutNode();
    tty.groupName = QStringLiteral("tty");
    check(serialGroupRemedy(tty).isEmpty(), "never recommend joining tty");

    SerialDeviceProbe allowed = deniedDialoutNode();
    allowed.accessErrno = 0;
    check(serialGroupRemedy(allowed).isEmpty(), "accessible node: no group advice");

    SerialOpenFailure f;
    f.kind = SerialOpenFailureKind::Permission;
    f.device = QStringLiteral("/dev/ttyUSB0");
    f.error = QStringLiteral("Permission denied");
    const QString advice = serialOpenFailureAdvice(f);
    check(!advice.contains(QStringLiteral("usermod")),
          "permission without a known remedy gives no usermod command");
    check(advice.contains(QStringLiteral("/dev/ttyUSB0")), "inspect advice names the device");
}

void testNotFound()
{
    SerialDeviceProbe gone;
    gone.statErrno = ENOENT;
    check(classifySerialOpenFailure(gone, false, false, 0) == SerialOpenFailureKind::NotFound,
          "ENOENT from stat is not found");
    check(classifySerialOpenFailure(SerialDeviceProbe{}, true, false, 0)
              == SerialOpenFailureKind::NotFound,
          "Qt DeviceNotFoundError is not found");

    SerialOpenFailure f;
    f.kind = SerialOpenFailureKind::NotFound;
    f.device = QStringLiteral("/dev/ttyUSB7");
    f.error = QStringLiteral("No such file or directory");
    check(!serialOpenFailureAdvice(f).contains(QStringLiteral("usermod")),
          "not found gives no group advice");
}

void testInUse()
{
    SerialDeviceProbe ok;
    ok.exists = true;   // accessible
    check(classifySerialOpenFailure(ok, false, true, 0) == SerialOpenFailureKind::InUse,
          "Qt PermissionError on an accessible node is a lock/exclusive hold");
    check(classifySerialOpenFailure(ok, false, true, EBUSY) == SerialOpenFailureKind::InUse,
          "EBUSY is in use");

    SerialOpenFailure f;
    f.kind = SerialOpenFailureKind::InUse;
    f.device = QStringLiteral("/dev/ttyUSB0");
    f.error = QStringLiteral("Permission error while locking the device");
    const QString advice = serialOpenFailureAdvice(f);
    check(!advice.contains(QStringLiteral("usermod"))
              && !advice.contains(QStringLiteral("dialout")),
          "in-use gives no misleading dialout advice");
}

void testOtherAndWin32()
{
    SerialDeviceProbe ok;
    ok.exists = true;
    check(classifySerialOpenFailure(ok, false, false, EIO) == SerialOpenFailureKind::Other,
          "an unrelated error is other");
    check(classifyWin32SerialOpenFailure(2) == SerialOpenFailureKind::NotFound, "Win32 2");
    check(classifyWin32SerialOpenFailure(5) == SerialOpenFailureKind::InUse, "Win32 5");
    check(classifyWin32SerialOpenFailure(32) == SerialOpenFailureKind::InUse, "Win32 32");
    check(classifyWin32SerialOpenFailure(31) == SerialOpenFailureKind::Other, "Win32 31");

    SerialOpenFailure f;
    f.kind = SerialOpenFailureKind::Other;
    f.device = QStringLiteral("/dev/ttyUSB0");
    f.error = QStringLiteral("Input/output error");
    check(serialOpenFailureSummary(f).startsWith(QStringLiteral("Input/output error")),
          "other keeps the underlying text verbatim");
    check(!serialOpenFailureAdvice(f).isEmpty(), "advice is never empty");
}

} // namespace

int main()
{
    testPermissionWithGroupRemedy();
    testNoGroupAdviceWhenItWouldNotHelp();
    testNotFound();
    testInUse();
    testOtherAndWin32();
    if (g_failures == 0)
        std::printf("serial_open_error_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
