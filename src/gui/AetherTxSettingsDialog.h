#pragma once

#include <QDialog>

class QLabel;
class QListWidget;
class QPushButton;

namespace AetherSDR {

class AetherTxProfiles;
class AudioEngine;

// The AetherTX window's settings: its profile library, plus the two live
// controls the old chain row carried.
//
// Save takes the transmit chain as it stands and names it; Load puts a stored
// one back; Export writes one to a JSON file to pass on; Import reads such a
// file and asks what to save it as, rather than overwriting whatever shares
// its name.
//
// The monitor record/playback pair moved here from the chain row the tab
// column replaced. It is not stranded: ClientChainApplet carries its own copy
// on the docked panel. BYPASS is not here -- it sits in the stage column
// above the Settings button, where a modal is not in the way of it.
class AetherTxSettingsDialog : public QDialog {
    Q_OBJECT

public:
    AetherTxSettingsDialog(AudioEngine* audio, QWidget* parent = nullptr);

signals:
    // A profile was applied — the host window re-reads the engine, since a
    // profile can reorder the chain and flip every stage's enable.
    void profileApplied();

    // Forwarded from the monitor pair, so the host keeps owning what they
    // mean. The strip already had these signals; the buttons simply live
    // here now.
    void monitorRecordClicked();
    void monitorPlayClicked();

public:
    // Host-driven state for the monitor pair — the strip receives these from
    // MainWindow and passes them through.
    void setMonitorRecording(bool on);
    void setMonitorPlaying(bool on);
    void setMonitorHasRecording(bool has);

private:
    void refreshList();
    QString selectedName() const;
    void updateButtonStates();

    void onSave();
    void onLoad();
    void onDelete();
    void onExport();
    void onImport();

    AetherTxProfiles* m_profiles{nullptr};
    QPushButton*      m_monRecBtn{nullptr};
    QPushButton*      m_monPlayBtn{nullptr};
    QListWidget*      m_list{nullptr};
    QPushButton*      m_saveBtn{nullptr};
    QPushButton*      m_loadBtn{nullptr};
    QPushButton*      m_deleteBtn{nullptr};
    QPushButton*      m_exportBtn{nullptr};
    QPushButton*      m_importBtn{nullptr};
    QLabel*           m_status{nullptr};
};

} // namespace AetherSDR
