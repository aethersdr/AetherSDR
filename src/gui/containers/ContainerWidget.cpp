#include "ContainerWidget.h"
#include "ContainerTitleBar.h"

#include <QDrag>
#include <QMimeData>
#include <QPixmap>
#include <QVBoxLayout>

namespace AetherSDR {

ContainerWidget::ContainerWidget(const QString& id, const QString& title,
                                 QWidget* parent)
    : QWidget(parent)
    , m_id(id)
{
    // objectName mirrors the stable container id ("AG", "AMP", "SS", …) so
    // the agent automation bridge can scope title-bar controls to a specific
    // container, e.g. invoke "AG/containerFloatToggle". (#3646)
    setObjectName(id);

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
        restoreWidthPolicy(previous);
        m_bodyLayout->removeWidget(previous);
        previous->setParent(nullptr);
    }
    m_content = content;
    if (m_content && m_bodyLayout) {
        m_content->setParent(m_body);
        m_bodyLayout->addWidget(m_content, 1);
        m_content->show();
        applyWidthPolicyTo(m_content);
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
    applyWidthPolicyTo(child);
}

void ContainerWidget::removeChildWidget(QWidget* child)
{
    if (!child || !m_bodyLayout) return;
    restoreWidthPolicy(child);
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
    // Re-apply the width policy to every body child for the new mode:
    // floating lifts width caps so content fills the window; docking
    // restores each child's original cap. (#3451)
    if (m_bodyLayout) {
        for (int i = 0; i < m_bodyLayout->count(); ++i) {
            if (QWidget* child = m_bodyLayout->itemAt(i)->widget())
                applyWidthPolicyTo(child);
        }
    }
    emit dockModeChanged(mode);
}

void ContainerWidget::applyWidthPolicyTo(QWidget* child)
{
    if (!child) return;
    if (m_dockMode == DockMode::Floating) {
        // Remember the docked cap once, then lift it so width-capped
        // applets fill the floating window instead of hugging the left
        // edge.  Uncapped content (maximumWidth == QWIDGETSIZE_MAX) is a
        // no-op here. (#3451)
        if (!m_savedMaxWidths.contains(child))
            m_savedMaxWidths.insert(child, child->maximumWidth());
        child->setMaximumWidth(QWIDGETSIZE_MAX);
    } else {
        restoreWidthPolicy(child);
    }
}

void ContainerWidget::restoreWidthPolicy(QWidget* child)
{
    if (child && m_savedMaxWidths.contains(child))
        child->setMaximumWidth(m_savedMaxWidths.take(child));
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

    // MIME type is shared with AppletDropArea's drag-reorder handling.
    auto* drag = new QDrag(m_titleBar);
    auto* mime = new QMimeData;
    // Note: the MIME id here is the container's drag id (m_id by default).
    // For composite tiles whose AppletEntry.id differs from m_id (e.g.
    // TXDSP wraps a tx_dsp container), AppletPanel::dropEvent has an
    // aliasing fallback that catches the mismatch by looking up the
    // container instead of the AppletEntry. See #1836.
    mime->setData("application/x-aethersdr-applet", dragId().toUtf8());
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
