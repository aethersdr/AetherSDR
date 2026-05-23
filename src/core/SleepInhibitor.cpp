#include "SleepInhibitor.h"
#include <QDebug>

#ifdef Q_OS_MAC
#include <IOKit/pwr_mgt/IOPMLib.h>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#if defined(Q_OS_LINUX) && defined(HAVE_DBUS)
#include <QDBusInterface>
#include <QDBusReply>
#endif

namespace MasterSDR {

SleepInhibitor::~SleepInhibitor()
{
    release();
}

void SleepInhibitor::acquire(const QString& reason)
{
    if (m_held)
        return;

#ifdef Q_OS_MAC
    CFStringRef cfReason = reason.toCFString();
    IOReturn ret = IOPMAssertionCreateWithName(
        kIOPMAssertionTypeNoIdleSleep,
        kIOPMAssertionLevelOn,
        cfReason,
        &m_assertionId);
    CFRelease(cfReason);
    if (ret == kIOReturnSuccess) {
        m_held = true;
        qDebug() << "SleepInhibitor: acquired (macOS IOPMAssertion)";
    } else {
        qWarning() << "SleepInhibitor: IOPMAssertionCreate failed:" << ret;
    }
#endif

#ifdef Q_OS_WIN
    EXECUTION_STATE prev = SetThreadExecutionState(
        ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    if (prev != 0 || true) { // SetThreadExecutionState returns previous state, 0 only on error
        m_held = true;
        qDebug() << "SleepInhibitor: acquired (Windows SetThreadExecutionState)";
    }
#endif

#if defined(Q_OS_LINUX) && defined(HAVE_DBUS)
    QDBusInterface iface("org.freedesktop.ScreenSaver",
                         "/org/freedesktop/ScreenSaver",
                         "org.freedesktop.ScreenSaver");
    if (iface.isValid()) {
        QDBusReply<uint32_t> reply = iface.call("Inhibit", "MasterSDR", reason);
        if (reply.isValid()) {
            m_cookie = reply.value();
            m_held = true;
            qDebug() << "SleepInhibitor: acquired (D-Bus ScreenSaver.Inhibit)";
        } else {
            qWarning() << "SleepInhibitor: D-Bus Inhibit failed:" << reply.error().message();
        }
    } else {
        qWarning() << "SleepInhibitor: D-Bus ScreenSaver interface not available";
    }
#endif

    Q_UNUSED(reason);
}

void SleepInhibitor::release()
{
    if (!m_held)
        return;

#ifdef Q_OS_MAC
    IOPMAssertionRelease(m_assertionId);
    m_assertionId = 0;
    qDebug() << "SleepInhibitor: released (macOS)";
#endif

#ifdef Q_OS_WIN
    SetThreadExecutionState(ES_CONTINUOUS);
    qDebug() << "SleepInhibitor: released (Windows)";
#endif

#if defined(Q_OS_LINUX) && defined(HAVE_DBUS)
    QDBusInterface iface("org.freedesktop.ScreenSaver",
                         "/org/freedesktop/ScreenSaver",
                         "org.freedesktop.ScreenSaver");
    if (iface.isValid())
        iface.call("UnInhibit", m_cookie);
    m_cookie = 0;
    qDebug() << "SleepInhibitor: released (Linux D-Bus)";
#endif

    m_held = false;
}

} // namespace MasterSDR
