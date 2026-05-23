#pragma once

#ifdef HAVE_MIDI

#include "PersistentDialog.h"

#include <QTableWidget>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>

namespace MasterSDR {

class MidiControlManager;

// MIDI Mapping dialog — dedicated settings window for configuring MIDI
// controller bindings. Opened from Settings → MIDI Mapping.
// Shows device selector, binding table with Learn mode, and profile
// save/load. All bindings stored in ~/.config/MasterSDR/midi.settings.
class MidiMappingDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit MidiMappingDialog(MidiControlManager* manager, QWidget* parent = nullptr);

private:
    void refreshPortList();
    void refreshBindingTable();
    void refreshProfileList();

    MidiControlManager* m_manager;

    QComboBox*    m_portCombo;
    QPushButton*  m_connectBtn;
    QLabel*       m_statusLabel;
    QLabel*       m_activityLabel;
    QTableWidget* m_bindingTable;
    QComboBox*    m_paramCombo;
    QComboBox*    m_categoryCombo;
    QComboBox*    m_profileCombo;
};

} // namespace MasterSDR

#endif // HAVE_MIDI
