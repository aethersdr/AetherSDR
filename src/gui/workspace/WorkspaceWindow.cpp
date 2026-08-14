#include "gui/workspace/WorkspaceWindow.h"

#include "gui/workspace/WorkspaceCanvas.h"

#include "core/AppSettings.h"
#include "gui/FramelessResizer.h"
#include "gui/FramelessWindowTitleBar.h"
#include "gui/Theme.h"

#include <QCloseEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {
// First-open size when there is no hint: generous enough to arrange in,
// small enough to fit any monitor the anchor lives on.
constexpr int kDefaultW = 960;
constexpr int kDefaultH = 600;
}  // namespace

WorkspaceWindow::WorkspaceWindow(const QString& surfaceId, QWidget* parent)
    : QWidget(parent, Qt::Window)
    , m_surfaceId(surfaceId)
{
    // The FloatingContainerWindow frameless contract, verbatim: honour the
    // app-wide setting, and re-apply via setWindowFlags — some platform
    // plugins ignore the constructor bitmask and need the explicit call
    // before show().
    const bool frameless =
        AppSettings::instance().value("FramelessWindow", "True").toString()
        == "True";
    Qt::WindowFlags flags = Qt::Window;
    if (frameless) flags |= Qt::FramelessWindowHint;
    setWindowFlags(flags);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAttribute(Qt::WA_QuitOnClose, false);
    setAttribute(Qt::WA_StyledBackground, true);
    applyAppTheme(this);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);

    // Move/close chrome (8600 field report: a frameless canvas window had
    // no way to be moved at all — FramelessResizer only owns the edges).
    // FramelessWindowTitleBar brings drag-to-move via FramelessMoveHelper
    // (the #4725 native-Wayland path), min/max/close, and double-click
    // maximize.  Its close button calls window()->close(), which lands in
    // closeEvent below — so chrome-close IS hide-and-keep, same as the
    // WM's.  Native decorations already provide all of this, so the bar
    // exists only in frameless mode.
    if (frameless) {
        m_titleBar = new FramelessWindowTitleBar(surfaceId, this);
        m_layout->addWidget(m_titleBar);
    }

    m_canvas = new WorkspaceCanvas(this);
    // dumpTree/grab targeting: every canvas needs a distinct name once
    // there is more than one.
    m_canvas->setObjectName(QStringLiteral("WorkspaceCanvas:%1").arg(surfaceId));
    m_layout->addWidget(m_canvas, 1);

    m_hintTimer.setSingleShot(true);
    m_hintTimer.setInterval(400);
    connect(&m_hintTimer, &QTimer::timeout, this, [this] {
        emit geometryHintChanged(m_surfaceId, saveGeometry());
    });

    FramelessResizer::install(this);
}

void WorkspaceWindow::setSurfaceLabel(const QString& label)
{
    const QString title = label.isEmpty() ? m_surfaceId : label;
    setWindowTitle(title);
    if (m_titleBar) {
        m_titleBar->setTitleText(title);
    }
}

void WorkspaceWindow::restoreFromHint(const QByteArray& hint, QWidget* anchor)
{
    m_restoring = true;
    // A minimized window must come back when asked (red-team #4971 N2):
    // show() is a no-op on an iconified window and saveGeometry() carries
    // only maximized/fullScreen, so the Canvas Windows menu and the
    // bridge's `window open` could never un-minimize — the only in-app
    // controls for this window, dead against it.
    setWindowState(windowState() & ~Qt::WindowMinimized);
    bool restored = !hint.isEmpty() && restoreGeometry(hint);
    if (!restored) {
        resize(kDefaultW, kDefaultH);
        QScreen* screen = anchor && anchor->screen()
                              ? anchor->screen()
                              : QGuiApplication::primaryScreen();
        if (screen) {
            const QRect g = screen->availableGeometry();
            move(g.center().x() - kDefaultW / 2,
                 g.center().y() - kDefaultH / 2);
        }
    } else {
        // Clamp to a connected screen: the hint may name a monitor that is
        // no longer there, and an off-screen canvas window reads as a lost
        // arrangement even though every item rect is intact.
        bool onScreen = false;
        const QPoint tl = geometry().topLeft();
        for (QScreen* s : QGuiApplication::screens()) {
            if (s->availableGeometry().contains(tl)) {
                onScreen = true;
                break;
            }
        }
        if (!onScreen) {
            QScreen* screen = anchor && anchor->screen()
                                  ? anchor->screen()
                                  : QGuiApplication::primaryScreen();
            if (screen) {
                const QRect g = screen->availableGeometry();
                move(g.center().x() - width() / 2,
                     g.center().y() - height() / 2);
            }
        }
    }
    m_restoring = false;
}

QByteArray WorkspaceWindow::currentGeometryHint() const
{
    return saveGeometry();
}

void WorkspaceWindow::prepareShutdown()
{
    m_hintTimer.stop();
    emit geometryHintChanged(m_surfaceId, saveGeometry());
    m_shuttingDown = true;
    close();
}

void WorkspaceWindow::closeEvent(QCloseEvent* ev)
{
    if (m_shuttingDown) {
        ev->accept();
        return;
    }
    // Never act locally: the controller must evict the surface's widgets
    // BEFORE the window disappears (hide-and-keep), and only it knows how.
    ev->ignore();
    emit closeRequested(m_surfaceId);
}

void WorkspaceWindow::moveEvent(QMoveEvent* ev)
{
    QWidget::moveEvent(ev);
    if (!m_restoring) m_hintTimer.start();
}

void WorkspaceWindow::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    if (!m_restoring) m_hintTimer.start();
}

}  // namespace AetherSDR
