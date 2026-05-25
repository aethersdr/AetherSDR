#pragma once

#include "PersistentDialog.h"
#include "core/CatPort.h"

#include <QList>

class QCheckBox;
class QLineEdit;
class QComboBox;
class QLabel;
class QGridLayout;
class QScrollArea;

namespace AetherSDR {

// Popped-out CAT configuration window.
//
// Shows one row per CatPort with:
//   [En] [Port] [Dialect] [VFO A] [VFO B] [PTY path] [Clients]
//
// The enable checkbox freezes all other controls while the port is running
// (matching SmartSDR for Mac UX). Emits configChanged() whenever any setting
// is modified; MainWindow connects this to applyCatPortCount().
class CatPopoutWindow : public PersistentDialog {
    Q_OBJECT

public:
    explicit CatPopoutWindow(QWidget* parent = nullptr);

    // Called by MainWindow to wire the ports and set the slice count.
    void setPorts(CatPort** ports, int count);
    void setMaxSlices(int n);

    // Sync the enable toggle state from the master enable in the applet.
    void setMasterEnabled(bool on);

signals:
    void configChanged();

private:
    void buildTable();
    void rebuildRows();
    void updateRowEnabled(int row, bool portRunning);
    void applyRowToSettings(int row);
    void populateVfoCombo(QComboBox* combo, bool includeNone);

    static constexpr int kMaxPorts = 8;

    struct PortRow {
        QCheckBox* enableCheck{nullptr};
        QLineEdit* portEdit{nullptr};
        QComboBox* dialectCombo{nullptr};
        QComboBox* vfoACombo{nullptr};
        QComboBox* vfoBCombo{nullptr};
        QLabel*    ptyLabel{nullptr};
        QLabel*    clientLabel{nullptr};
    };

    CatPort*  m_ports[kMaxPorts]{};
    int       m_portCount{0};
    int       m_maxSlices{2};
    bool      m_masterEnabled{false};

    QGridLayout* m_grid{nullptr};
    QList<PortRow> m_rows;
};

} // namespace AetherSDR
