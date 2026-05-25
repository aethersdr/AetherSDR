#pragma once

#include "core/CatPort.h"

#include <QWidget>
#include <QList>

class QPushButton;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QLabel;
class QVBoxLayout;
class QGridLayout;
class QIntValidator;

namespace AetherSDR {

// CAT Control Applet — docked and floating views in one widget.
//
// Docked:   [Enable CAT]  "Pop out for configuration."
// Floating: per-port table (En / Port / Dialect / VFO A / VFO B / PTY / Clients)
//
// AppletPanel calls setFloating(true/false) when the ContainerWidget's dock
// mode changes. MainWindow calls setPorts() / setMaxSlices() after construction.
// configChanged() fires whenever any per-port setting is saved; MainWindow
// connects this to applyCatPortCount().
class CatControlApplet : public QWidget {
    Q_OBJECT

public:
    explicit CatControlApplet(QWidget* parent = nullptr);

    // Wire the backing CatPort objects. Call before the applet is shown.
    void setPorts(CatPort** ports, int count);

    // Update how many slices are available (scales VFO A/B combos).
    void setMaxSlices(int n);

    // Switch between docked (simple) and floating (full table) views.
    void setFloating(bool on);

    // Sync the master enable button without firing enableChanged.
    void setCatEnabled(bool on);

signals:
    void enableChanged(bool on);  // master toggle
    void configChanged();         // any per-port setting changed

private:
    void buildDockedView(QWidget* page);
    void buildTableRows();
    void populateVfoCombo(QComboBox* combo, bool includeNone);
    void applyRowToSettings(int row);
    void updateRowLocked(int row);

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
    int       m_maxSlices{kMaxPorts};  // show all letters pre-connection; capped to hw max on connect

    QVBoxLayout*    m_rootLayout{nullptr};
    QWidget*        m_dockedPage{nullptr};
    QWidget*        m_floatingPage{nullptr};   // nullptr until first setFloating(true)
    QPushButton*    m_enableBtn{nullptr};
    QPushButton*    m_floatingEnableBtn{nullptr};

    // Floating view internals
    QGridLayout*    m_grid{nullptr};
    QList<PortRow>  m_rows;
    bool            m_rowsBuilt{false};
};

} // namespace AetherSDR
