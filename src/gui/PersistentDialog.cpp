#include "PersistentDialog.h"
#include "FramelessResizer.h"
#include "FramelessWindowTitleBar.h"
#include "core/AppSettings.h"

#include <QCloseEvent>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QVBoxLayout>

namespace MasterSDR {

PersistentDialog::PersistentDialog(const QString& title,
                                   const QString& geomKey,
                                   QWidget* parent)
    : QDialog(parent), m_geomKey(geomKey)
{
    setWindowTitle(title);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_titleBar = new FramelessWindowTitleBar(title, this);
    outer->addWidget(m_titleBar);

    // Body widget is created here, but its layout is owned by the subclass:
    //   auto* root = new QVBoxLayout(bodyWidget());
    // setFramelessMode() reaches into bodyWidget()->layout() to nudge the top
    // margin when frameless chrome is on, so subclasses don't need to know
    // about the frameless toggle.
    m_body = new QWidget(this);
    outer->addWidget(m_body, 1);

    FramelessResizer::install(this);
    setFramelessMode(
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True");
}

void PersistentDialog::setFramelessMode(bool on)
{
    m_framelessOn = on;

    const QRect geom = geometry();
    const bool wasVisible = isVisible();

    Qt::WindowFlags flags = (windowFlags() & ~Qt::WindowType_Mask) | Qt::Dialog;
    flags.setFlag(Qt::FramelessWindowHint, on);
    setWindowFlags(flags);
    if (wasVisible) {
        setGeometry(geom);
    }

    if (m_titleBar) {
        m_titleBar->setVisible(on);
    }
    applyBodyLayoutMargins();
    if (wasVisible) {
        show();
    }
}

// Nudge the subclass-owned body layout: 2 px tighter at the top when frameless
// chrome is on, so the custom title bar sits flush against the content rather
// than gaining a double-margin look.  No-op when the subclass hasn't installed
// its layout yet (base-ctor call); first showEvent() re-applies once it has.
void PersistentDialog::applyBodyLayoutMargins()
{
    if (m_body && m_body->layout()) {
        m_body->layout()->setContentsMargins(9, m_framelessOn ? 7 : 9, 9, 9);
    }
}

void PersistentDialog::showEvent(QShowEvent* event)
{
    // Restore geometry on first show only.  Deferring past the constructor
    // lets subclasses call setMinimumSize() in their own ctor without the
    // just-restored geometry being clipped by a later minimum.
    if (!m_geometryRestored) {
        m_geometryRestored = true;
        restoreGeometryFromSettings();
        // The base-ctor setFramelessMode() ran before the subclass installed
        // its body layout, so the frameless top-margin nudge was a no-op.
        // Re-apply now that the layout exists.
        applyBodyLayoutMargins();
    }
    QDialog::showEvent(event);
}

void PersistentDialog::closeEvent(QCloseEvent* event)
{
    saveGeometryToSettings();
    // Move/resize saves are in-memory only; close-time save flushes to disk.
    // Matches the crash-resilient persistence contract established by
    // ProfileManagerDialog: if MasterSDR is force-quit, the last known
    // position is already in AppSettings even if the disk flush never ran.
    if (!m_geomKey.isEmpty()) {
        AppSettings::instance().save();
    }
    QDialog::closeEvent(event);
}

void PersistentDialog::moveEvent(QMoveEvent* event)
{
    QDialog::moveEvent(event);
    // Suppress saves before the first restore has run.  Native-window creation
    // on first show() delivers moveEvent/resizeEvent *before* showEvent — if
    // those saves weren't gated they would overwrite the prior session's
    // saved geometry with the default geometry, defeating persistence.
    if (m_geometryRestored && !m_restoringGeometry) saveGeometryToSettings();
}

void PersistentDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    if (m_geometryRestored && !m_restoringGeometry) saveGeometryToSettings();
}

// AppSettings serializes via XML strings, so window geometry round-trips
// through base64 to preserve binary data.  Matches the project convention
// used by MainWindow{Geometry,State} and ProfileManagerDialog.
void PersistentDialog::saveGeometryToSettings()
{
    if (m_geomKey.isEmpty()) return;
    AppSettings::instance().setValue(m_geomKey, saveGeometry().toBase64());
}

void PersistentDialog::restoreGeometryFromSettings()
{
    if (m_geomKey.isEmpty()) return;
    const QString geomB64 = AppSettings::instance().value(m_geomKey).toString();
    if (geomB64.isEmpty()) return;
    // Restore triggers move/resize events; flag those as restore-driven so
    // they don't immediately overwrite the value we just loaded.
    m_restoringGeometry = true;
    restoreGeometry(QByteArray::fromBase64(geomB64.toLatin1()));
    m_restoringGeometry = false;
}

} // namespace MasterSDR
