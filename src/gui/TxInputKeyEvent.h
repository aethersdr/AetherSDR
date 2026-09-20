#pragma once

#include <QKeyEvent>
#include "models/TxController.h"

namespace AetherSDR {

// Typed internal event payload: automation still traverses the application's
// real event filters, but a synthesized edge cannot borrow native key state.
// The controller is captured before delivery, never discovered in the filter.
class TxInputKeyEvent final : public QKeyEvent {
public:
    TxInputKeyEvent(QEvent::Type type, int key, Qt::KeyboardModifiers modifiers,
                    std::shared_ptr<TxController> source)
        : QKeyEvent(type, key, modifiers), controller(std::move(source)) {}
    const std::shared_ptr<TxController> controller;
};

} // namespace AetherSDR
