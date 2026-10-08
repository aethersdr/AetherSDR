#include "StatusIndicator.h"

#include "core/ThemeManager.h"

#include <QAccessible>
#include <QAccessibleActionInterface>
#include <QAccessibleWidget>
#include <QEvent>
#include <QKeyEvent>
#include <QVariant>
#include <QWidget>

namespace AetherSDR {

namespace {

constexpr const char* kHelperProperty = "aetherStatusIndicator";
constexpr int kFocusBarHeight = 2;

// Reports an indicator as a Button whose Press action runs the indicator.
class StatusIndicatorAccessible : public QAccessibleWidget {
public:
    explicit StatusIndicatorAccessible(QWidget* widget)
        : QAccessibleWidget(widget, QAccessible::Button)
    {
    }

    QStringList actionNames() const override
    {
        return QStringList{pressAction()} + QAccessibleWidget::actionNames();
    }

    void doAction(const QString& actionName) override
    {
        if (actionName == pressAction()) {
            if (StatusIndicator* helper = StatusIndicator::of(widget())) {
                helper->activate();
            }
            return;
        }
        QAccessibleWidget::doAction(actionName);
    }
};

QAccessibleInterface* statusIndicatorFactory(const QString&, QObject* object)
{
    auto* widget = qobject_cast<QWidget*>(object);
    if (widget && StatusIndicator::of(widget)) {
        return new StatusIndicatorAccessible(widget);
    }
    return nullptr;
}

}  // namespace

StatusIndicator* StatusIndicator::attach(QWidget* widget)
{
    if (!widget) {
        return nullptr;
    }
    if (StatusIndicator* existing = of(widget)) {
        return existing;
    }
    static const bool factoryInstalled = [] {
        QAccessible::installFactory(statusIndicatorFactory);
        return true;
    }();
    Q_UNUSED(factoryInstalled);
    auto* helper = new StatusIndicator(widget);
    // With accessibility already active, Qt may have created and cached the
    // widget's plain interface (setAccessibleName() does so). The cache is
    // consulted before factories, so drop it; the next query builds the
    // Button. ObjectCreated tells an assistive client that already saw the
    // old object to look again.
    if (QAccessible::isActive()) {
        if (QAccessibleInterface* cached = QAccessible::queryAccessibleInterface(widget)) {
            if (cached->role() != QAccessible::Button) {
                QAccessible::deleteAccessibleInterface(QAccessible::uniqueId(cached));
                QAccessibleEvent created(widget, QAccessible::ObjectCreated);
                QAccessible::updateAccessibility(&created);
            }
        }
    }
    return helper;
}

StatusIndicator* StatusIndicator::of(const QWidget* widget)
{
    return widget ? widget->property(kHelperProperty).value<StatusIndicator*>() : nullptr;
}

StatusIndicator::StatusIndicator(QWidget* widget)
    : QObject(widget), m_widget(widget), m_focusBar(new QWidget(widget))
{
    widget->setProperty(kHelperProperty, QVariant::fromValue(this));
    widget->setFocusPolicy(Qt::TabFocus);

    // A child bar rather than a stylesheet rule: the indicators restyle
    // themselves with whole-sheet setStyleSheet() calls on every state change.
    m_focusBar->setObjectName(QStringLiteral("statusIndicatorFocusBar"));
    m_focusBar->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_focusBar->setFocusPolicy(Qt::NoFocus);
    ThemeManager::instance().applyStyleSheet(
        m_focusBar, QStringLiteral("QWidget { background: {{color.canon.cyan}}; border: none; }"));
    m_focusBar->hide();

    widget->installEventFilter(this);
}

void StatusIndicator::activate()
{
    if (m_widget->isEnabled()) {
        emit activated();
    }
}

void StatusIndicator::placeFocusBar()
{
    m_focusBar->setGeometry(0, m_widget->height() - kFocusBarHeight,
                            m_widget->width(), kFocusBarHeight);
    m_focusBar->raise();
}

bool StatusIndicator::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != m_widget) {
        return QObject::eventFilter(watched, event);
    }
    switch (event->type()) {
    case QEvent::KeyPress: {
        const auto* keyEvent = static_cast<QKeyEvent*>(event);
        const int key = keyEvent->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) {
            // One action per press: these toggle panels and cycle TUN/AMP, so
            // a held key's auto-repeat is consumed, not acted on.
            if (!keyEvent->isAutoRepeat()) {
                activate();
            }
            return true;
        }
        break;
    }
    case QEvent::FocusIn:
        placeFocusBar();
        m_focusBar->show();
        break;
    case QEvent::FocusOut:
        m_focusBar->hide();
        break;
    case QEvent::Resize:
        placeFocusBar();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

}  // namespace AetherSDR
