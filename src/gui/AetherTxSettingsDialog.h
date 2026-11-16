#pragma once

#include "CanonWindow.h"

#include <functional>

class QLabel;
class QListWidget;
class QPushButton;

namespace AetherSDR {

class AetherTxProfiles;
class AudioEngine;

// The AetherTX window's settings: its profile library.
//
// Save takes the transmit chain as it stands and names it; Load puts a stored
// one back; Export writes one to a JSON file to pass on; Import reads such a
// file and asks what to save it as, rather than overwriting whatever shares
// its name.
//
// Nothing live is in here. Record, Play and BYPASS sit in the stage column
// beside the Settings gear, where a modal is not in the way of them.
// The profile library behind AetherTX's Settings gear: a short-lived
// CanonWindow dialog (style guide, RFC #6226), modal over its window.
//
// The stock presets are listed first and are read-only: they load and export
// but cannot be saved over or deleted. Loading one also asks the host, through
// the StockTxFilterHandler, to set the radio TX filter the preset was tuned
// for; the handler says what happened, for the status line.
class AetherTxSettingsDialog : public CanonWindow {
    Q_OBJECT

public:
    // Sets the radio TX filter to lowHz..highHz if it can. Returns whether it
    // did; `note` says what the operator should know either way.
    using StockTxFilterHandler = std::function<bool(int lowHz, int highHz, QString* note)>;

    AetherTxSettingsDialog(AudioEngine* audio, QWidget* parent = nullptr,
                           StockTxFilterHandler txFilterHandler = {});

signals:
    // A profile was applied — the host window re-reads the engine, since a
    // profile can reorder the chain and flip every stage's enable.
    void profileApplied();

private:
    void refreshList();
    QString selectedName() const;
    bool selectedIsStock() const;
    void updateButtonStates();

    void onSave();
    void onLoad();
    void onDelete();
    void onExport();
    void onImport();

    AetherTxProfiles* m_profiles{nullptr};
    StockTxFilterHandler m_txFilterHandler;
    QListWidget*      m_list{nullptr};
    QPushButton*      m_saveBtn{nullptr};
    QPushButton*      m_loadBtn{nullptr};
    QPushButton*      m_deleteBtn{nullptr};
    QPushButton*      m_exportBtn{nullptr};
    QPushButton*      m_importBtn{nullptr};
    QLabel*           m_status{nullptr};
};

} // namespace AetherSDR
