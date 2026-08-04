#pragma once

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
    Headless,   // DRM connectors exist, but none is "connected"
    Unknown     // no DRM connectors found — we cannot tell
};

// Scan the DRM connector status files under `drmRoot`. Returns:
//   Connected — some connector's status is exactly "connected";
//   Headless  — connectors exist but none is connected (e.g. a Pi over wayvnc);
//   Unknown   — no connector directories were found (no /sys/class/drm, an
//               unusual driver, a container) — the caller should not assume
//               headless in that case.
// Writeback / virtual connectors report "unknown" and so never count as
// connected. `drmRoot` is a parameter (not hard-coded) so the logic is testable
// against a fake sysfs tree; it defaults to the real path.
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
    for (const QString& c : connectors) {
        QFile status(drmRoot + QLatin1Char('/') + c + QStringLiteral("/status"));
        // status is a single short word; bound the read.
        if (status.open(QIODevice::ReadOnly)
            && status.readLine(64).trimmed() == "connected") {
            return DisplayPresence::Connected;
        }
    }
    return DisplayPresence::Headless;
}

}  // namespace AetherSDR
