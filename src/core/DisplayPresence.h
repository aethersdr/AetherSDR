#pragma once

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QLatin1Char>
#include <QString>
#include <QStringList>

namespace AetherSDR {

// Whether a physical display is attached, as reported by the Linux DRM
// subsystem. Used to choose the Qt platform on a Wayland session: a headless
// session has no scanout, so native-Wayland hardware GL cannot allocate a
// window surface and we prefer XWayland (see detectDisplayPresence use in
// main.cpp).
enum class DisplayPresence {
    Connected,  // at least one DRM connector reports "connected"
    Headless,   // at least one connector is "disconnected", and none "connected"
    Unknown     // no connectors, or every connector reports "unknown"
};

// Scan the DRM connector status files under `drmRoot`. Returns:
//   Connected — some connector's status is exactly "connected";
//   Headless  — at least one connector is "disconnected" and none is connected
//               (e.g. a Pi over wayvnc with HDMI unplugged);
//   Unknown   — no connector directories were found (no /sys/class/drm, an
//               unusual driver, a container), OR every connector reports
//               "unknown" — the caller should not assume headless in that case.
// "unknown" is not only writeback/virtual connectors: a connector that cannot do
// hotplug detection (DPI, composite TV-out, some fixed DSI panels) also reports
// "unknown" while a panel is physically attached. Requiring an explicit
// "disconnected" before claiming Headless keeps such a display off XWayland,
// matching the tri-state rule: do not assume headless when we cannot tell.
// `drmRoot` is a parameter (not hard-coded) so the logic is testable against a
// fake sysfs tree; it defaults to the real path.
inline DisplayPresence detectDisplayPresence(
    const QString& drmRoot = QStringLiteral("/sys/class/drm"))
{
    QDir drm(drmRoot);
    const QStringList connectors = drm.entryList(
        QStringList{QStringLiteral("card*-*")},
        QDir::Dirs | QDir::NoDotAndDotDot);
    if (connectors.isEmpty()) {
        return DisplayPresence::Unknown;
    }
    bool sawDisconnected = false;
    for (const QString& c : connectors) {
        QFile status(drmRoot + QLatin1Char('/') + c + QStringLiteral("/status"));
        // status is a single short word; bound the read.
        if (!status.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QByteArray s = status.readLine(64).trimmed();
        if (s == "connected") {
            return DisplayPresence::Connected;
        }
        if (s == "disconnected") {
            sawDisconnected = true;
        }
    }
    // No connector said "connected". Only claim Headless if at least one said
    // "disconnected"; an all-"unknown" set (writeback/virtual, or a panel that
    // can't hotplug-detect) means we cannot tell.
    return sawDisconnected ? DisplayPresence::Headless : DisplayPresence::Unknown;
}

}  // namespace AetherSDR
