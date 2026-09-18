#pragma once

#include <QDialog>

class QLabel;
class QListWidget;
class QPushButton;

namespace AetherSDR {

class AetherRxProfiles;
class AudioEngine;

// The AetherRX window's settings: its profile library.
//
// Save takes the receive chain as it stands and names it; Load puts a stored
// one back; Export writes one to a JSON file to pass on; Import reads such a
// file and asks what to save it as, rather than overwriting whatever shares
// its name.
class AetherRxSettingsDialog : public QDialog {
    Q_OBJECT

public:
    AetherRxSettingsDialog(AudioEngine* audio, QWidget* parent = nullptr);

signals:
    // A profile was applied — the host window re-reads the engine, since a
    // profile can reorder the chain and flip every stage's enable.
    void profileApplied();

private:
    void refreshList();
    QString selectedName() const;
    void updateButtonStates();

    void onSave();
    void onLoad();
    void onDelete();
    void onExport();
    void onImport();

    AetherRxProfiles* m_profiles{nullptr};
    QListWidget*      m_list{nullptr};
    QPushButton*      m_saveBtn{nullptr};
    QPushButton*      m_loadBtn{nullptr};
    QPushButton*      m_deleteBtn{nullptr};
    QPushButton*      m_exportBtn{nullptr};
    QPushButton*      m_importBtn{nullptr};
    QLabel*           m_status{nullptr};
};

} // namespace AetherSDR
