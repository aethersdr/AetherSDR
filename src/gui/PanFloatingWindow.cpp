#include "PanFloatingWindow.h"
#include "FramelessResizer.h"
#include "PanadapterApplet.h"
#include "Theme.h"
#include "core/AppSettings.h"

#include <QCloseEvent>
#include <QVBoxLayout>

namespace AetherSDR {

PanFloatingWindow::PanFloatingWindow(QWidget* parent)
    : QWidget(parent, Qt::Window)
{
    const bool frameless =
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True";
    Qt::WindowFlags flags = Qt::Window;
    if (frameless) flags |= Qt::FramelessWindowHint;
    // Re-apply via setWindowFlags — some platform plugins ignore the
    // constructor bitmask and need an explicit call before show().
    setWindowFlags(flags);
    setMinimumSize(400, 300);
    setAttribute(Qt::WA_StyledBackground, true);
    applyAppTheme(this);

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);

    FramelessResizer::install(this);
}

void PanFloatingWindow::adoptApplet(PanadapterApplet* applet)
{
    if (!applet) return;
    m_applet = applet;

    // Use the user-facing slice title (e.g. "Slice A") instead of raw hex pan ID
    QString title = applet->sliceTitle();
    if (title.isEmpty())
        title = QString("Pan %1").arg(applet->panId());
    setWindowTitle(QString("AetherSDR — %1").arg(title));

    // Reparent directly into this window — addWidget() calls setParent()
    // internally, so the widget goes straight from the splitter to the
    // floating window without an intermediate nullptr/top-level state.
    // This avoids corrupting the main window's NSResponder chain on macOS.
    m_layout->addWidget(m_applet, 1);

    // Show dock icon in the applet's title bar
    m_applet->setFloatingState(true);
    connect(m_applet, &PanadapterApplet::dockClicked, this, [this]() {
        emit dockRequested(panId());
    });
}

QString PanFloatingWindow::panId() const
{
    return m_applet ? m_applet->panId() : QString();
}

PanadapterApplet* PanFloatingWindow::takeApplet()
{
    if (!m_applet) return nullptr;
    auto* a = m_applet;
    m_layout->removeWidget(a);
    a->setParent(nullptr);
    m_applet = nullptr;
    return a;
}

void PanFloatingWindow::setFramelessMode(bool on)
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
    if (wasVisible) setGeometry(geom);
    if (wasVisible) show();
}

void PanFloatingWindow::closeEvent(QCloseEvent* ev)
{
    if (m_shuttingDown) {
        ev->accept();
        return;
    }
    // Don't close — dock instead
    emit dockRequested(panId());
    ev->ignore();
}

void PanFloatingWindow::saveWindowGeometry()
{
    auto& s = AppSettings::instance();
    s.setValue(QString("FloatingPan_%1_Geometry").arg(panId()),
              QString::fromLatin1(saveGeometry().toBase64()));
}

void PanFloatingWindow::restoreWindowGeometry()
{
    auto& s = AppSettings::instance();
    QString geom = s.value(QString("FloatingPan_%1_Geometry").arg(panId())).toString();
    if (!geom.isEmpty()) {
        restoreGeometry(QByteArray::fromBase64(geom.toLatin1()));
    }
}

} // namespace AetherSDR
