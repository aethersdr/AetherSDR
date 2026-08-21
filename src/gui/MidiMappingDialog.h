#pragma once

#ifdef HAVE_MIDI

#include "PersistentDialog.h"

#include <QTableWidget>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>

namespace AetherSDR {

class MidiControlManager;
struct MidiBinding;

// MIDI Mapping dialog — dedicated settings window for configuring MIDI
// controller bindings. Opened from Settings → MIDI Mapping.
// Shows device selector, binding table with Learn mode, and profile
// save/load. All bindings stored in ~/.config/AetherSDR/midi.settings.
class MidiMappingDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit MidiMappingDialog(MidiControlManager* manager, QWidget* parent = nullptr);

private:
    void refreshPortList();
    void refreshBindingTable();
    void refreshProfileList();
    // Manual add/edit form (#4760). One form serves both the "Manual…" button
    // (existing == nullptr) and the per-row edit button (existing == the row's
    // current binding). Commits through the same addBinding()/save path as Learn.
    void openManualEditor(const QString& paramId, const MidiBinding* existing);
    // Profile file import/export (#4888). Import accepts the native
    // <MidiProfile> XML or a SmartSDR iOS/Mac ".map" (auto-detected), stores
    // a new profile (suffix on name collision) and selects it in the combo;
    // Export writes the current bindings as a shareable profile XML.
    void importProfileFromFile();
    void exportProfileToFile();

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

} // namespace AetherSDR

#endif // HAVE_MIDI
