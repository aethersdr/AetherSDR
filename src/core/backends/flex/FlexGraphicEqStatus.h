#pragma once

#include "core/backends/AudioControlRequest.h"

#include <QMap>
#include <QString>

namespace AetherSDR {

// A Flex "eq txsc …" / "eq rxsc …" status's key/values → GraphicEqStatus:
// mode=1 enables the path, "<band>Hz=<dB>" sets a band. Header-only so tests
// run the same parse.
inline GraphicEqStatus flexGraphicEqStatus(GraphicEqRequest::Path path, const QMap<QString, QString>& kvs)
{
    static constexpr int kBandsHz[] = {63, 125, 250, 500, 1000, 2000, 4000, 8000};
    GraphicEqStatus status;
    status.path = path;
    if (kvs.contains(QStringLiteral("mode"))) { status.enabled = kvs.value(QStringLiteral("mode")) == QLatin1String("1"); }
    for (int i = 0; i < 8; ++i) {
        const QString key = QStringLiteral("%1Hz").arg(kBandsHz[i]);
        if (kvs.contains(key)) { status.bands[i] = kvs.value(key).toInt(); }
    }
    return status;
}

} // namespace AetherSDR
