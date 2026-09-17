#pragma once

#include "core/TxKeyingMarker.h"

#include <QCheckBox>

namespace AetherSDR {

// Capture automatic-response permission before queued widget delivery. The
// absence of a bridge TX grant means RX-only, never a native input fallback.
inline void registerModemReceiveAction(QCheckBox* box,
    std::function<void(bool, const std::shared_ptr<TxController>&,
                       const TxController::Input&)> apply)
{
    registerReceiveControlAction(box, [box, apply = std::move(apply)](
        const std::shared_ptr<TxController>& controller, const QString& action,
        const QString& value) -> TxKeyingAction::Prepared {
        if (action != QLatin1String("click") && action != QLatin1String("toggle")
            && action != QLatin1String("setChecked")) {
            return {};
        }
        const QString normalized = value.trimmed().toLower();
        const bool enabled = action == QLatin1String("setChecked")
            ? normalized == QLatin1String("true") || normalized == QLatin1String("1")
                || normalized == QLatin1String("on") || normalized == QLatin1String("yes")
            : !box->isChecked();
        const TxController::Input input = controller
            ? controller->captureProgram(TxController::Activity::Mox) : TxController::Input{};
        return [apply, controller, input, enabled] {
            if (controller && !input.valid()) { return; }
            apply(enabled, controller, input);
        };
    });
}

} // namespace AetherSDR
