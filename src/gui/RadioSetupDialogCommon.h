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
#ifdef HAVE_SERIALPORT
#include <QSerialPortInfo>
#endif

namespace AetherSDR {

inline const QString kGroupStyle =
    "QGroupBox { border: 1px solid #304050; border-radius: 4px; "
    "margin-top: 8px; padding-top: 12px; font-weight: bold; color: #8aa8c0; }"
    "QGroupBox::title { subcontrol-origin: margin; left: 10px; "
    "padding: 0 4px; }";

// Caption label style (#5896): tokens, applied only through
// ThemeManager::applyStyleSheet, which tracks widgets for re-resolution on
// themeChanged. Named *Template because a {{token}} string must never reach
// setStyleSheet(), which does not expand it.
inline const QString kLabelStyleTemplate =
    "QLabel { color: {{color.text.primary}}; font-size: 12px; }";

// Line-edit style as tokens; each literal it replaced was that token's Default
// Dark value (resources/themes/default-dark.json), so only Light changes.
inline const QString kEditStyleTemplate =
    "QLineEdit { background: {{color.background.1}}; "
    "border: 1px solid {{color.background.2}}; border-radius: 3px; "
    "color: {{color.text.primary}}; font-size: 12px; padding: 2px 4px; }";

// One call shape for every caption label and line edit routed through the two
// templates above, so the widget is tracked for re-resolution as well as styled.
inline void applyLabelStyle(QWidget* widget)
{
    ThemeManager::instance().applyStyleSheet(widget, kLabelStyleTemplate);
}

inline void applyEditStyle(QWidget* widget)
{
    ThemeManager::instance().applyStyleSheet(widget, kEditStyleTemplate);
}

// Uses ThemeManager tokens (Low Latency architecture) with hover + disabled
// pseudo-states (FreeDV Reporter pattern) so boxes are visible in dark mode.
inline const QString kCheckBoxIndicator =
    "QCheckBox::indicator { width: 14px; height: 14px; "
    "border: 2px solid {{color.background.3}}; border-radius: 3px; background: {{color.background.0}}; }"
    "QCheckBox::indicator:hover { border-color: {{color.accent}}; background: {{color.background.1}}; }"
    "QCheckBox::indicator:checked { border: 2px solid {{color.accent}}; background: {{color.background.2}}; }"
    "QCheckBox::indicator:disabled { border-color: {{color.background.2}}; background: {{color.background.0}}; }";

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
