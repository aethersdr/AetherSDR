#pragma once

// Styles and helpers shared by the Radio Setup dialog's translation units
// (RadioSetupDialog.cpp and RadioSetupDialog_Peripherals.cpp). Internal to the
// dialog; not a public interface.

#include "SerialPortCombo.h"
#include "core/ThemeManager.h"

#include <QAccessible>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QString>
#include <QStyle>
#include <QWidget>
#ifdef HAVE_SERIALPORT
#include <QSerialPortInfo>
#endif

namespace AetherSDR {

// Radio Setup is a canon window: one sheet on its body (kRadioSetupStyle in
// RadioSetupDialog.cpp) styles every page. Widgets carry a role for it rather
// than a sheet of their own.

// Text tones, styled by QLabel[rsTone="..."]:
//   Caption  a field's name ("Callsign:")       Value    a radio-reported value
//   Hint     help and secondary text            Heading  a section or sub-group title
//   Title    a page's device name               Accent   a status in progress
//   Ok / Warn / Error  a settled status
enum class Tone { Caption, Value, Hint, Heading, Title, Ok, Warn, Error, Accent };

// A status chip: a small bordered label that reads on (green) or off.
enum class Chip { Off, On };

// Button roles, styled by QPushButton[rsButton="..."]. A plain checkable
// button is an on/off toggle (green when on); Select is one of a set (cyan
// when chosen); Mute reads red when engaged; Danger and Success are actions
// with consequences (reboot, remove, upload firmware).
enum class ButtonRole { Select, Mute, Danger, Success };

namespace detail {
inline void setRoleProperty(QWidget* widget, const char* name, const char* value)
{
    if (widget->property(name).toByteArray() == value) {
        return;
    }
    widget->setProperty(name, QByteArray(value));
    // A property selector is matched at polish time: re-polish so a role
    // changed on a live widget (a status that turns to an error) restyles.
    if (widget->style()) {
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
    }
    widget->update();
}
} // namespace detail

inline void setTone(QWidget* widget, Tone tone)
{
    static constexpr const char* kNames[] = {
        "caption", "value", "hint", "heading", "title", "ok", "warn", "error", "accent"};
    detail::setRoleProperty(widget, "rsTone", kNames[static_cast<int>(tone)]);
}

inline void setChip(QWidget* widget, Chip chip)
{
    detail::setRoleProperty(widget, "rsChip", chip == Chip::On ? "on" : "off");
}

inline void setButtonRole(QWidget* button, ButtonRole role)
{
    static constexpr const char* kNames[] = {"select", "mute", "danger", "success"};
    detail::setRoleProperty(button, "rsButton", kNames[static_cast<int>(role)]);
}

// Whether a label already has a tone; the groups' caption sweeps leave those.
inline bool hasTone(const QWidget* widget)
{
    return widget->property("rsTone").isValid() || widget->property("rsChip").isValid();
}

// The caption every label beside a field takes.
inline void applyLabelStyle(QWidget* widget)
{
    setTone(widget, Tone::Caption);
}

// The control port the ShackSwitch connect path connects on and saves its
// credential under. Removal derives its endpoint from the same value.
inline constexpr quint16 kShackSwitchControlPort = 9007;

inline void showRemovalNotice(QLabel* notice, const QString& text)
{
    notice->setText(text);
    notice->setAccessibleDescription(text);
    notice->show();
    for (const QAccessible::Event type : {QAccessible::NameChanged,
                                          QAccessible::DescriptionChanged}) {
        QAccessibleEvent event(notice, type);
        QAccessible::updateAccessibility(&event);
    }
}

#ifdef HAVE_SERIALPORT
// Enumeration stays in the GUI's deferred page/show paths. The shared helper
// accepts a port list so selection and signal behavior can be tested without
// serial hardware.
inline bool populateSerialPortCombo(QComboBox* combo, QLineEdit* customEdit,
                                    const QString& savedPort)
{
    return SerialPortCombo::populate(combo, customEdit, savedPort,
                                     QSerialPortInfo::availablePorts());
}

inline bool refreshSerialPortCombo(QComboBox* combo, QLineEdit* customEdit)
{
    return SerialPortCombo::refresh(combo, customEdit, [] {
        return QSerialPortInfo::availablePorts();
    });
}
#endif

} // namespace AetherSDR
