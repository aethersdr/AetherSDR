#pragma once

#include <QWidget>
#include <QPointer>
#include <QVariant>
#include <utility>
#include "models/TxController.h"

namespace AetherSDR {

// Authoritative marker for controls that *key the transmitter* when activated —
// MOX/PTT, TUNE, ATU tune, CWX CW send, AX.25 packet send, and any future
// keying control. The #3646 automation bridge refuses invoke() on any widget
// carrying this marker unless AETHER_AUTOMATION_ALLOW_TX is set, so an agent can
// never key a live radio by accident during hardware-in-the-loop tests.
//
// This is a *positive* signal set at the keying control's creation site, which
// is far more robust than matching control names: a TX-capable control is
// guarded because it was explicitly declared keying, not because its label
// happened to contain a magic word. Name matching remains in the bridge only as
// a logged belt-and-suspenders fallback for controls that predate or forget the
// marker.
//
// Usage at the call site, right after creating the button:
//     m_moxBtn = new QPushButton("MOX");
//     markTxKeying(m_moxBtn);
inline constexpr char kTxKeyingProperty[] = "aetherTxKeying";

// A control supplies its real controller action, not a label-based model
// shortcut or an ambient "current actor" around QWidget::click(). Preparation
// captures input identity BEFORE the bridge queues invocation. The same UI
// logic remains responsible for toggles, text and per-frequency ATU behavior.
struct TxKeyingAction {
    using Prepared = std::function<void()>;
    using Prepare = std::function<Prepared(const std::shared_ptr<TxController>&,
                                           const QString&, const QString&)>;
    Prepare prepare;
    // Receive controls may optionally establish an automatic-response program.
    // Without a TX controller their prepared action must remain receive-only.
    bool requiresTxPermission{true};
};
inline constexpr char kTxKeyingActionProperty[] = "aetherTxKeyingAction";

inline void markTxKeying(QWidget* w)
{
    if (w)
        w->setProperty(kTxKeyingProperty, true);
}

} // namespace AetherSDR

Q_DECLARE_METATYPE(std::shared_ptr<const AetherSDR::TxKeyingAction>)

namespace AetherSDR {

inline void registerTxKeyingAction(QObject* object, TxKeyingAction::Prepare prepare)
{
    if (!object) {
        return;
    }
    object->setProperty(kTxKeyingProperty, true);
    object->setProperty(kTxKeyingActionProperty,
        QVariant::fromValue(std::make_shared<const TxKeyingAction>(TxKeyingAction{std::move(prepare)})));
}

inline void registerReceiveControlAction(QObject* object, TxKeyingAction::Prepare prepare)
{
    if (!object) { return; }
    object->setProperty(kTxKeyingProperty, true);
    object->setProperty(kTxKeyingActionProperty,
        QVariant::fromValue(std::make_shared<const TxKeyingAction>(
            TxKeyingAction{std::move(prepare), false})));
}

inline bool txActionRequiresPermission(const QObject* object)
{
    const auto endpoint = object ? object->property(kTxKeyingActionProperty)
        .value<std::shared_ptr<const TxKeyingAction>>() : nullptr;
    return !endpoint || endpoint->requiresTxPermission;
}

inline TxKeyingAction::Prepared prepareTxKeyingAction(QObject* object,
    const std::shared_ptr<TxController>& controller, const QString& action, const QString& value)
{
    if (!object || (controller && !controller->valid())) {
        return {};
    }
    const std::shared_ptr<const TxKeyingAction> endpoint =
        object->property(kTxKeyingActionProperty).value<std::shared_ptr<const TxKeyingAction>>();
    if (!endpoint || !endpoint->prepare || (endpoint->requiresTxPermission && !controller)) {
        return {};
    }
    const QPointer<QObject> guard(object);
    TxKeyingAction::Prepared prepared = endpoint->prepare(controller, action, value);
    if (!prepared) {
        return {};
    }
    return [guard, controller, prepared = std::move(prepared)] {
        if (guard && (!controller || controller->valid())) {
            if (const QWidget* widget = qobject_cast<QWidget*>(guard.data()); widget && !widget->isEnabled()) {
                return;
            }
            prepared();
        }
    };
}

// A pointer activation of a TX control uses the same registered action as
// invoke(), with its input captured before delivery. Never send a raw click
// into a native operator callback. Ordinary non-TX widgets still receive Qt
// events through the bridge. Preserve button hit-testing, focus and down state;
// only an explicit release inside may activate, never cancellation/timeout.
class TxPointerAction final {
public:
    static std::shared_ptr<TxPointerAction> prepare(QWidget* hit,
        const std::shared_ptr<TxController>& controller);
    ~TxPointerAction();
    void press(const QPoint& global);
    void move(const QPoint& global);
    void release(const QPoint& global);
    void cancel();
    std::shared_ptr<TxController> controller() const { return m_controller; }

private:
    void clearDownState();
    bool hits(const QPoint& global) const;
    QPointer<QWidget> m_button;
    std::shared_ptr<TxController> m_controller;
    TxKeyingAction::Prepared m_action;
    bool m_started{false};
};

} // namespace AetherSDR
