#include "FloatingContainerWindow.h"
#include "ContainerWidget.h"
#include "core/AppSettings.h"
#include "gui/FramelessResizer.h"
#include "gui/Theme.h"

#include <QByteArray>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

constexpr int kDefaultW = 300;
constexpr int kDefaultH = 240;

} // namespace

FloatingContainerWindow::FloatingContainerWindow(QWidget* parent)
    : QWidget(parent, Qt::Window)
{
    const bool frameless =
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True";
    Qt::WindowFlags flags = Qt::Window;
    if (frameless) flags |= Qt::FramelessWindowHint;
    // Re-apply via setWindowFlags — some platform plugins ignore the
    // constructor bitmask and need an explicit call before show().
    setWindowFlags(flags);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAttribute(Qt::WA_QuitOnClose, false);
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(darkThemeStylesheet());
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(400);
    connect(&m_saveTimer, &QTimer::timeout, this,
            &FloatingContainerWindow::saveGeometryToKey);

    FramelessResizer::install(this);
}

FloatingContainerWindow::~FloatingContainerWindow() = default;

void FloatingContainerWindow::takeContainer(ContainerWidget* container)
{
    if (m_container == container) return;

    if (m_container) {
        m_layout->removeWidget(m_container);
        m_container->setParent(nullptr);
    }
    m_container = container;
    if (m_container) {
        m_container->setParent(this);
        m_layout->addWidget(m_container, 1);
        m_container->show();
        m_container->setDockMode(ContainerWidget::DockMode::Floating);
        setWindowTitle(m_container->title());
    }
}

ContainerWidget* FloatingContainerWindow::releaseContainer()
{
    ContainerWidget* c = m_container;
    if (c) {
        m_layout->removeWidget(c);
        c->setParent(nullptr);
        c->setDockMode(ContainerWidget::DockMode::PanelDocked);
        m_container = nullptr;
    }
    return c;
}

void FloatingContainerWindow::setGeometryKey(const QString& key)
{
    m_geometryKey = key;
}

void FloatingContainerWindow::restoreAndEnsureVisible(QWidget* anchor)
{
    m_restoring = true;
    bool restored = false;
    if (!m_geometryKey.isEmpty()) {
        const QByteArray geom = QByteArray::fromBase64(
            AppSettings::instance()
                .value(m_geometryKey, "").toByteArray());
        if (!geom.isEmpty() && restoreGeometry(geom)) {
            restored = true;
        }
    }
    if (!restored) {
        // Default: size to the container's natural sizeHint so we don't
        // open with dead space above and below the children.  Width
        // falls back to kDefaultW when the hint is too narrow to read.
        adjustSize();
        const QSize hint = sizeHint();
        const int w = std::max(kDefaultW, hint.width());
        const int h = std::max(hint.height(), 80);  // tiny floor
        resize(w, h);
        QScreen* screen = anchor && anchor->screen()
            ? anchor->screen()
            : QGuiApplication::primaryScreen();
        if (screen) {
            const QRect g = screen->availableGeometry();
            move(g.center().x() - w / 2,
                 g.center().y() - h / 2);
        }
    } else {
        // Clamp to any visible screen — saved geometry may reference
        // a monitor that's no longer connected.
        bool onScreen = false;
        const QPoint tl = geometry().topLeft();
        for (QScreen* s : QGuiApplication::screens()) {
            if (s->availableGeometry().contains(tl)) { onScreen = true; break; }
        }
        if (!onScreen) {
            QScreen* screen = anchor && anchor->screen()
                ? anchor->screen() : QGuiApplication::primaryScreen();
            if (screen) {
                const QRect g = screen->availableGeometry();
                move(g.center().x() - width() / 2,
                     g.center().y() - height() / 2);
            }
        }
    }
    m_restoring = false;
}

void FloatingContainerWindow::saveGeometryToKey() const
{
    if (m_geometryKey.isEmpty()) return;
    AppSettings::instance().setValue(
        m_geometryKey, saveGeometry().toBase64());
}

void FloatingContainerWindow::prepareShutdown()
{
    m_saveTimer.stop();
    saveGeometryToKey();
    m_shuttingDown = true;
    close();
}

void FloatingContainerWindow::setFramelessMode(bool on)
{
    const bool wasVisible = isVisible();
    const QRect geom = geometry();
    Qt::WindowFlags flags = windowFlags();
    if (on) {
        flags |= Qt::FramelessWindowHint;
    } else {
        flags &= ~Qt::FramelessWindowHint;
    }
    setWindowFlags(flags);
    setGeometry(geom);
    if (wasVisible) show();
}

void FloatingContainerWindow::setAlwaysOnTop(bool on)
{
    if (m_alwaysOnTop == on) return;
    m_alwaysOnTop = on;
    const bool wasVisible = isVisible();
    const QRect geom = geometry();
    Qt::WindowFlags flags = windowFlags();
    if (on) {
        flags |= Qt::WindowStaysOnTopHint;
    } else {
        flags &= ~Qt::WindowStaysOnTopHint;
    }
    setWindowFlags(flags);
    setGeometry(geom);
    if (wasVisible) show();
}

void FloatingContainerWindow::closeEvent(QCloseEvent* ev)
{
    if (m_shuttingDown) {
        ev->accept();
        return;
    }
    // Close = dock.  Manager handles reparenting via dockRequested.
    if (m_container) {
        emit dockRequested(m_container);
    }
    QWidget::closeEvent(ev);
}

void FloatingContainerWindow::moveEvent(QMoveEvent* ev)
{
    QWidget::moveEvent(ev);
    if (!m_restoring) m_saveTimer.start();
}

void FloatingContainerWindow::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    if (!m_restoring) m_saveTimer.start();
}

} // namespace AetherSDR
