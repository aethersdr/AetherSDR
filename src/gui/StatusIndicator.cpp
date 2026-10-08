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

    QAccessible::State state() const override
    {
        QAccessible::State s = QAccessibleWidget::state();
        if (const StatusIndicator* helper = StatusIndicator::of(widget())) {
            s.checkable = helper->isCheckable();
            s.checked = helper->isCheckable() && helper->isChecked();
        }
        return s;
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

QAccessibleInterface* statusIndicatorFactory(const QString& className, QObject* object)
{
    // Qt asks every factory once per level of the object's class chain; only
    // the most-derived level can be an indicator, so every other level (and
    // every other object in the app) skips the property lookup.
    if (!object || className != QLatin1String(object->metaObject()->className())) {
        return nullptr;
    }
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
    // Button. ObjectDestroyed then ObjectCreated tell an assistive client that
    // already saw the old object to let it go and look again.
    if (QAccessible::isActive()) {
        if (QAccessibleInterface* cached = QAccessible::queryAccessibleInterface(widget)) {
            if (cached->role() != QAccessible::Button) {
                // Retire the old object for clients holding its id, then
                // announce the Button that replaces it.
                QAccessibleEvent destroyed(cached, QAccessible::ObjectDestroyed);
                QAccessible::updateAccessibility(&destroyed);
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

void StatusIndicator::setCheckable(bool checkable)
{
    m_checkable = checkable;
}

void StatusIndicator::setChecked(bool checked)
{
    if (m_checked == checked) {
        return;
    }
    m_checked = checked;
    if (m_checkable && QAccessible::isActive()) {
        QAccessible::State changed;
        changed.checked = true;
        QAccessibleStateChangeEvent event(m_widget, changed);
        QAccessible::updateAccessibility(&event);
    }
}

void StatusIndicator::setCheckedFor(QWidget* widget, bool checked)
{
    if (StatusIndicator* helper = of(widget)) {
        helper->setChecked(checked);
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
        // Not Space: MainWindow's app-level filter binds it to PTT (Hold), and
        // it runs before this one, so Space would key the radio, not this.
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
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
