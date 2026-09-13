#include "core/TxKeyingMarker.h"

#include <QCheckBox>
#include <QPushButton>
#include <QStyleOptionButton>

namespace AetherSDR {

std::shared_ptr<TxPointerAction> TxPointerAction::prepare(QWidget* hit,
    const std::shared_ptr<TxController>& controller)
{
    QWidget* endpoint = hit;
    while (endpoint && !endpoint->property(kTxKeyingActionProperty).isValid()) {
        endpoint = endpoint->parentWidget();
    }
    QAbstractButton* button = qobject_cast<QAbstractButton*>(endpoint);
    if (!button || (!qobject_cast<QPushButton*>(button) && !qobject_cast<QCheckBox*>(button))) {
        return {};
    }
    const std::shared_ptr<TxController> scope = TxController::captureInputScope(controller);
    if (controller && !scope) { return {}; }
    const QPointer<QAbstractButton> guard(button);
    TxKeyingAction::Prepared action = prepareTxKeyingAction(button, scope, QStringLiteral("click"), {});
    if (!guard || !action) { return {}; }
    auto result = std::make_shared<TxPointerAction>();
    result->m_button = guard;
    result->m_controller = scope;
    result->m_action = std::move(action);
    return result;
}

TxPointerAction::~TxPointerAction() { cancel(); }

void TxPointerAction::press(const QPoint& global)
{
    m_started = hits(global);
    if (!m_started) { return; }
    const QPointer<QAbstractButton> button = qobject_cast<QAbstractButton*>(m_button.data());
    if (button->focusPolicy() & Qt::ClickFocus) {
        button->setFocus(Qt::MouseFocusReason);
    }
    if (button && (!m_controller || m_controller->valid())) {
        button->setDown(true);
    }
}

void TxPointerAction::move(const QPoint& global)
{
    if (QAbstractButton* button = qobject_cast<QAbstractButton*>(m_button.data())) {
        button->setDown(m_started && hits(global));
    }
}

void TxPointerAction::release(const QPoint& global)
{
    const bool activate = m_started && hits(global);
    TxKeyingAction::Prepared action = std::exchange(m_action, {});
    clearDownState();
    if (!activate && m_controller) { m_controller->invalidate(); }
    if (activate && action) { action(); }
}

void TxPointerAction::cancel()
{
    clearDownState();
    if (std::exchange(m_action, {}) && m_controller) {
        m_controller->invalidate();
    }
}

void TxPointerAction::clearDownState()
{
    if (QAbstractButton* button = qobject_cast<QAbstractButton*>(m_button.data()); m_started && button) {
        button->setDown(false);
    }
    m_started = false;
}

bool TxPointerAction::hits(const QPoint& global) const
{
    if (!m_button || !m_button->isEnabled() || (m_controller && !m_controller->valid())) {
        return false;
    }
    QRect area = m_button->rect();
    if (const QCheckBox* box = qobject_cast<QCheckBox*>(m_button.data())) {
        QStyleOptionButton option;
        option.initFrom(box);
        option.text = box->text();
        option.icon = box->icon();
        option.iconSize = box->iconSize();
        option.state |= box->isChecked() ? QStyle::State_On : QStyle::State_Off;
        area = box->style()->subElementRect(QStyle::SE_CheckBoxClickRect, &option, box);
    }
    return area.contains(m_button->mapFromGlobal(global));
}

} // namespace AetherSDR
