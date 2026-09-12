#pragma once

#include "core/backends/IRadioBackend.h"

#include <atomic>

namespace AetherSDR {

// The client may only access this extension on a connected, capable ANAN.
// Keep this check shared by the dialog and automation so a retained page
// cannot issue commands to another family's current backend.
inline bool droopCalibrationAvailable(const IRadioBackend* backend)
{
    if (!backend || !backend->isConnected()) {
        return false;
    }
    const RadioCapabilities caps = backend->capabilities();
    return caps.family == QLatin1String("anan") && caps.hostDroopCalibration;
}

// ANAN's local calibration commands reply synchronously. Progress is separate:
// extensionStatus("anan", "droop", fields). No concrete backend types escape.
inline QVariantMap requestDroopCalibration(IRadioBackend* backend, const QString& action)
{
    if (!droopCalibrationAvailable(backend)) {
        return {{QStringLiteral("ok"), false},
                {QStringLiteral("error"), QStringLiteral("droopcal: connect a radio with ANAN droop calibration support")}};
    }
    static std::atomic<quint64> nextId{quint64{1} << 63};
    const quint64 id = nextId.fetch_add(1, std::memory_order_relaxed);
    QVariantMap reply{{QStringLiteral("ok"), false},
                      {QStringLiteral("error"), QStringLiteral("droopcal: backend did not answer")}};
    // Stack context removes both connections on every return path.
    QObject context;
    QObject::connect(backend, &IRadioBackend::extensionResult, &context,
        [&reply, id](quint64 resultId, const QVariant& result) {
            if (resultId == id) {
                reply = result.toMap();
                reply.insert(QStringLiteral("ok"), true);
            }
        });
    QObject::connect(backend, &IRadioBackend::extensionError, &context,
        [&reply, id](quint64 resultId, const QString& reason) {
            if (resultId == id) {
                reply = {{QStringLiteral("ok"), false},
                         {QStringLiteral("error"), QStringLiteral("droopcal: %1").arg(reason)}};
            }
        });
    backend->invokeExtension(QStringLiteral("anan"), QStringLiteral("droop.%1").arg(action), id, {});
    return reply;
}

} // namespace AetherSDR
