#include "ContainerWidget.h"
#include "ContainerTitleBar.h"

#include <QDrag>
#include <QMimeData>
#include <QPixmap>
#include <QVBoxLayout>
#include <QWindow>

namespace AetherSDR {

ContainerWidget::ContainerWidget(const QString& id, const QString& title,
                                 QWidget* parent)
    : QWidget(parent)
    , m_id(id)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_titleBar = new ContainerTitleBar(title, this);
    outer->addWidget(m_titleBar);

    m_body = new QWidget(this);
    m_bodyLayout = new QVBoxLayout(m_body);
    m_bodyLayout->setContentsMargins(0, 0, 0, 0);
    m_bodyLayout->setSpacing(0);
    outer->addWidget(m_body, 1);

    connect(m_titleBar, &ContainerTitleBar::floatToggleClicked,
            this, &ContainerWidget::onTitleBarFloatToggle);
    connect(m_titleBar, &ContainerTitleBar::closeClicked,
            this, &ContainerWidget::onTitleBarClose);
    connect(m_titleBar, &ContainerTitleBar::alwaysOnTopToggled,
            this, &ContainerWidget::alwaysOnTopToggled);
    connect(m_titleBar, &ContainerTitleBar::dragStartRequested,
            this, &ContainerWidget::onTitleBarDragStart);
}

ContainerWidget::~ContainerWidget() = default;

void ContainerWidget::setTitle(const QString& title)
{
    if (m_titleBar) m_titleBar->setTitle(title);
}

QString ContainerWidget::title() const
{
    return m_titleBar ? m_titleBar->title() : QString();
}

QWidget* ContainerWidget::setContent(QWidget* content)
{
    QWidget* previous = m_content;
    if (previous && m_bodyLayout) {
        m_bodyLayout->removeWidget(previous);
        previous->setParent(nullptr);
    }
    m_content = content;
    if (m_content && m_bodyLayout) {
        m_content->setParent(m_body);
        m_bodyLayout->addWidget(m_content, 1);
        m_content->show();
    }
    return previous;
}

void ContainerWidget::insertChildWidget(int index, QWidget* child)
{
    if (!child || !m_bodyLayout) return;
    // If the child is already in this layout, leave it where AppletPanel
    // placed it.  Without this guard, ContainerManager::restoreState()'s
    // second pass would re-insert each saved child at indices 0..N-1,
    // displacing non-container peers (e.g. ClientChainApplet) that
    // AppletPanel inserted directly without recording them in the
    // ContainerTree JSON.  Drag-reorder persistence flows through the
    // packed-atomic chain order, not through this path.
    if (m_bodyLayout->indexOf(child) >= 0) return;
    child->setParent(m_body);
    if (index < 0 || index > m_bodyLayout->count())
        index = m_bodyLayout->count();
    m_bodyLayout->insertWidget(index, child);
    child->show();
}

void ContainerWidget::removeChildWidget(QWidget* child)
{
    if (!child || !m_bodyLayout) return;
    m_bodyLayout->removeWidget(child);
    child->setParent(nullptr);
}

int ContainerWidget::childWidgetCount() const
{
    return m_bodyLayout ? m_bodyLayout->count() : 0;
}

QWidget* ContainerWidget::childWidgetAt(int index) const
{
    if (!m_bodyLayout) return nullptr;
    if (index < 0 || index >= m_bodyLayout->count()) return nullptr;
    QLayoutItem* item = m_bodyLayout->itemAt(index);
    return item ? item->widget() : nullptr;
}

int ContainerWidget::indexOfChildWidget(QWidget* child) const
{
    if (!m_bodyLayout || !child) return -1;
    return m_bodyLayout->indexOf(child);
}

void ContainerWidget::setTitleBarVisible(bool visible)
{
    if (m_titleBar) m_titleBar->setVisible(visible);
}

void ContainerWidget::setContainerVisible(bool visible)
{
    if (visible == m_visible) return;
    m_visible = visible;
    setVisible(visible);
    emit visibilityChanged(visible);
}

void ContainerWidget::setDockMode(DockMode mode)
{
    if (mode == m_dockMode) return;
    m_dockMode = mode;
    if (m_titleBar) m_titleBar->setFloatingState(mode == DockMode::Floating);
    emit dockModeChanged(mode);
}

void ContainerWidget::onTitleBarFloatToggle()
{
    if (isFloating()) emit dockRequested();
    else              emit floatRequested();
}

void ContainerWidget::onTitleBarClose()
{
    emit closeRequested();
}

void ContainerWidget::onTitleBarDragStart(const QPoint& /*globalPos*/)
{
    if (!m_titleBar) return;

    // Floating window: title-bar drag moves the OS window via the Qt 6
    // cross-platform primitive that hands the move off to the compositor.
    // (Pop-out windows are frameless — no native title bar to grab.)
    if (m_dockMode == DockMode::Floating) {
        if (auto* w = window()) {
            if (auto* h = w->windowHandle()) {
                h->startSystemMove();
                return;
            }
        }
    }

    // MIME type is shared with AppletDropArea's drag-reorder handling.
    auto* drag = new QDrag(m_titleBar);
    auto* mime = new QMimeData;
    mime->setData("application/x-aethersdr-applet", m_id.toUtf8());
    drag->setMimeData(mime);

    // Drag pixmap: a semi-opaque snapshot of the titlebar strip so
    // the user sees what they're moving.
    QPixmap pixmap(m_titleBar->size());
    pixmap.fill(Qt::transparent);
    m_titleBar->render(&pixmap);
    drag->setPixmap(pixmap);
    drag->setHotSpot(QPoint(m_titleBar->width() / 2,
                            m_titleBar->height() / 2));
    drag->exec(Qt::MoveAction);
}

} // namespace AetherSDR
