#include "ThemeInspector.h"
#include "core/ThemeManager.h"

#include <QApplication>
#include <QCursor>
#include <QDialog>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWidget>

namespace AetherSDR {

// Frameless top-level overlay that draws a cyan outline around the widget
// under the cursor.  Click-through via WindowTransparentForInput so the
// global event filter remains the single source of mouse/keyboard events.
class ThemeInspectorOverlay : public QWidget {
public:
    ThemeInspectorOverlay()
        : QWidget(nullptr,
                  Qt::Tool
                  | Qt::FramelessWindowHint
                  | Qt::WindowStaysOnTopHint
                  | Qt::WindowTransparentForInput
                  | Qt::BypassWindowManagerHint)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor accent = ThemeManager::instance().color("color.accent");
        QColor fill = accent;
        fill.setAlpha(40);
        p.setBrush(fill);
        p.setPen(QPen(accent, 2));
        p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 3, 3);
    }
};

ThemeInspector::ThemeInspector(QDialog* editorDialog, QObject* parent)
    : QObject(parent), m_editor(editorDialog)
{}

ThemeInspector::~ThemeInspector()
{
    if (m_active) stop();
    delete m_overlay;
}

void ThemeInspector::start()
{
    if (m_active) return;
    m_active = true;

    if (!m_overlay) m_overlay = new ThemeInspectorOverlay;

    QApplication::setOverrideCursor(QCursor(Qt::CrossCursor));
    qApp->installEventFilter(this);
    emit activeChanged(true);
}

void ThemeInspector::stop()
{
    if (!m_active) return;
    m_active = false;
    qApp->removeEventFilter(this);
    QApplication::restoreOverrideCursor();
    if (m_overlay) m_overlay->hide();
    m_lastTarget.clear();
    emit activeChanged(false);
}

bool ThemeInspector::eventFilter(QObject* /*obj*/, QEvent* ev)
{
    if (!m_active) return false;

    // While the editor has spawned a modal child dialog (QColorDialog,
    // GradientEditorDialog, "Theme exists" confirm, …), pause overlay
    // tracking and let every event flow through unmolested.  The modal
    // owns the user's attention; once it closes the inspector resumes
    // on the next mouse move.
    if (QApplication::activeModalWidget()) {
        if (m_overlay && m_overlay->isVisible()) m_overlay->hide();
        m_lastTarget.clear();
        return false;
    }

    switch (ev->type()) {
    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(ev);
        updateOverlay(resolveTarget(me->globalPosition().toPoint()));
        return false;
    }
    case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (me->button() != Qt::LeftButton) return false;
        const QPoint gp = me->globalPosition().toPoint();
        QWidget* target = resolveTarget(gp);
        if (!target) {
            // Click on the editor dialog or its children — let the dialog
            // handle it normally (so the operator can re-toggle Inspect
            // off without it counting as a pick).
            return false;
        }
        const QPoint local = target->mapFromGlobal(gp);
        stop();
        emit widgetPicked(target, local);
        return true;  // eat the click so it doesn't reach the underlying widget
    }
    case QEvent::MouseButtonRelease: {
        // Eat the matching release so half a click doesn't slip through.
        auto* me = static_cast<QMouseEvent*>(ev);
        if (me->button() != Qt::LeftButton) return false;
        QWidget* target = QApplication::widgetAt(me->globalPosition().toPoint());
        if (target && belongsToEditor(target)) return false;
        return true;
    }
    case QEvent::KeyPress: {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Escape) {
            stop();
            emit canceled();
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

QWidget* ThemeInspector::resolveTarget(const QPoint& globalPos) const
{
    QWidget* w = QApplication::widgetAt(globalPos);
    if (!w) return nullptr;
    if (belongsToEditor(w)) return nullptr;
    if (m_overlay && (w == m_overlay || m_overlay->isAncestorOf(w))) return nullptr;
    return w;
}

bool ThemeInspector::belongsToEditor(QWidget* w) const
{
    if (!m_editor || !w) return false;
    if (w == m_editor) return true;
    if (m_editor->isAncestorOf(w)) return true;
    // Catches child popups owned by the editor (QColorDialog, etc.)
    QWidget* top = w->window();
    return top == m_editor;
}

void ThemeInspector::updateOverlay(QWidget* target)
{
    if (!m_overlay) return;
    if (!target) {
        m_overlay->hide();
        m_lastTarget.clear();
        return;
    }
    if (m_lastTarget == target && m_overlay->isVisible()) return;
    m_lastTarget = target;
    const QRect global(target->mapToGlobal(QPoint(0, 0)), target->size());
    m_overlay->setGeometry(global);
    if (!m_overlay->isVisible()) m_overlay->show();
    m_overlay->raise();
    m_overlay->update();
}

} // namespace AetherSDR
