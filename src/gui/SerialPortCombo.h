#pragma once

#include <QAbstractItemView>
#include <QComboBox>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QString>

namespace AetherSDR::SerialPortCombo {

inline bool isCustom(const QComboBox* combo)
{
    return combo && combo->currentData().toString() == QLatin1String("__custom__");
}

// Port entries provide portName() and description(), like QSerialPortInfo.
// Block both widgets: repopulation must not save settings or look like an
// operator edit. Callers apply the returned editor visibility themselves.
template<typename Ports>
bool populate(QComboBox* combo, QLineEdit* customEdit, const QString& savedPort,
              const Ports& ports, bool preserveEmptyCustom = false)
{
    const QSignalBlocker comboBlocker(combo);
    const QSignalBlocker editBlocker(customEdit);
    combo->clear();
    for (const auto& info : ports) {
        combo->addItem(QString("%1 — %2").arg(info.portName(), info.description()),
                       info.portName());
    }
    combo->addItem("Custom...", QStringLiteral("__custom__"));

    const int match = savedPort.isEmpty() ? -1 : combo->findData(savedPort);
    if (match >= 0 && match < combo->count() - 1) {
        combo->setCurrentIndex(match);
    } else if (!savedPort.isEmpty() || preserveEmptyCustom || ports.isEmpty()) {
        combo->setCurrentIndex(combo->count() - 1);
        if (customEdit) {
            customEdit->setText(savedPort);
        }
    }
    // An empty enumeration selects Custom even without a saved path.
    return isCustom(combo);
}

// Invoke enumeration only after checking the popup guard. Preserve identity,
// including the operator's explicit empty Custom choice, across refresh/show.
template<typename Enumerate>
bool refresh(QComboBox* combo, QLineEdit* customEdit, Enumerate enumerate)
{
    if (!combo) {
        return false;
    }
    if (combo->view()->isVisible()) {
        return isCustom(combo);
    }

    const bool wasCustom = isCustom(combo);
    QString keep = combo->currentData().toString();
    if (keep.isEmpty() || wasCustom) {
        keep = customEdit ? customEdit->text().trimmed() : QString();
    }
    return populate(combo, customEdit, keep, enumerate(), wasCustom && keep.isEmpty());
}

} // namespace AetherSDR::SerialPortCombo
