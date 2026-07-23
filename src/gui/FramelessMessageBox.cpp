#include "FramelessMessageBox.h"

#include "FramelessResizer.h"
#include "FramelessWindowTitleBar.h"
#include "core/AppSettings.h"

#include <QLayout>
#include <QResizeEvent>
#include <QShowEvent>

namespace AetherSDR {

FramelessMessageBox::FramelessMessageBox(QWidget* parent)
    : QMessageBox(parent)
{
    m_originalMargins = layout()->contentsMargins();
    m_titleBar = new FramelessWindowTitleBar(windowTitle(), this);
    m_titleBar->raise();
    FramelessResizer::install(this);
    setFramelessMode(
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True");
}

void FramelessMessageBox::setFramelessMode(bool on)
{
    const QRect geom = geometry();
    const bool wasVisible = isVisible();
    Qt::WindowFlags flags = (windowFlags() & ~Qt::WindowType_Mask) | Qt::Dialog;
    flags.setFlag(Qt::FramelessWindowHint, on);
    setWindowFlags(flags);
    if (wasVisible) {
        setGeometry(geom);
    }

    m_frameless = on;
    m_titleBar->setVisible(on);
    applyTitleBarMargins();
    positionTitleBar();
    if (wasVisible) {
        show();
    }
}

void FramelessMessageBox::applyTitleBarMargins()
{
    const int titleHeight = m_frameless ? m_titleBar->sizeHint().height() : 0;
    layout()->setContentsMargins(m_originalMargins.left(),
                                 m_originalMargins.top() + titleHeight,
                                 m_originalMargins.right(),
                                 m_originalMargins.bottom());
}

void FramelessMessageBox::resizeEvent(QResizeEvent* event)
{
    QMessageBox::resizeEvent(event);
    positionTitleBar();
}

void FramelessMessageBox::showEvent(QShowEvent* event)
{
    m_titleBar->setTitleText(windowTitle());
    // QMessageBox rebuilds its grid layout (and resets contents margins) on any
    // post-construction setIcon/setText/addButton/setStandardButtons call, which
    // discards the top-margin reservation made in the constructor. Re-apply it
    // here so the base showEvent's updateSize() sizes the box with the reserved
    // space and the title-bar overlay no longer clips the top of the content.
    applyTitleBarMargins();
    QMessageBox::showEvent(event);
    positionTitleBar();
}

void FramelessMessageBox::positionTitleBar()
{
    if (m_titleBar) {
        m_titleBar->setGeometry(0, 0, width(), m_titleBar->sizeHint().height());
        m_titleBar->raise();
    }
}

QMessageBox::StandardButton FramelessMessageBox::showMessage(
    Icon icon, QWidget* parent, const QString& title, const QString& text,
    StandardButtons buttons, StandardButton defaultButton)
{
    FramelessMessageBox box(parent);
    box.setIcon(icon);
    box.setWindowTitle(title);
    box.setText(text);
    box.setStandardButtons(buttons);
    if (defaultButton != NoButton) {
        box.setDefaultButton(defaultButton);
    }
    return static_cast<StandardButton>(box.exec());
}

QMessageBox::StandardButton FramelessMessageBox::information(
    QWidget* parent, const QString& title, const QString& text,
    StandardButtons buttons, StandardButton defaultButton)
{
    return showMessage(Information, parent, title, text, buttons, defaultButton);
}

QMessageBox::StandardButton FramelessMessageBox::warning(
    QWidget* parent, const QString& title, const QString& text,
    StandardButtons buttons, StandardButton defaultButton)
{
    return showMessage(Warning, parent, title, text, buttons, defaultButton);
}

QMessageBox::StandardButton FramelessMessageBox::critical(
    QWidget* parent, const QString& title, const QString& text,
    StandardButtons buttons, StandardButton defaultButton)
{
    return showMessage(Critical, parent, title, text, buttons, defaultButton);
}

QMessageBox::StandardButton FramelessMessageBox::question(
    QWidget* parent, const QString& title, const QString& text,
    StandardButtons buttons, StandardButton defaultButton)
{
    return showMessage(Question, parent, title, text, buttons, defaultButton);
}

} // namespace AetherSDR
