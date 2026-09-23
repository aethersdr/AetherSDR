#pragma once

#include "gui/ControlAvailabilityRegistry.h"
#include "models/RadioModel.h"
#include <QAction>
#include <functional>

namespace AetherSDR {

// Shared by the spectrum context menu and its offline UI test. The action
// asks the model; it cannot write a USB frequency or change receiver state.
class ReceiveCaptureAction final : public QAction {
public:
    ReceiveCaptureAction(RadioModel& model, std::function<QString()> panId, QObject* parent)
        : QAction(tr("Move capture away from DC"), parent)
    {
        setObjectName(QStringLiteral("receiveCapturePlacementAction"));
        auto* availability = new ControlAvailabilityRegistry(model, this);
        availability->registerAction(this,
            tr("Capture DC placement requires a connected, supported FM or FM-N receiver."),
            [](bool connected, const RadioCapabilities& caps) {
                return connected && caps.receiveCapturePlacement.has_value();
            });
        connect(this, &QAction::triggered, this, [&model, panId = std::move(panId)] {
            model.requestReceiveCaptureRecenter(panId());
        });
    }
};

} // namespace AetherSDR
