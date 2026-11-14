#pragma once

#include "core/hfgateway/HfGatewayConfig.h"

#include <QList>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QTreeWidget;
class QVBoxLayout;

namespace AetherSDR {

namespace HfGateway {
class HostPortEngine;
}

// AetherModem's "HF Gateway" tab, Stage 1 of RFC #7296: receive-only.
//
// The operator lists the external HF data modems this station runs (any
// modem with a VARA-style host interface: Mercury, data2g-host, or another
// VARA-compatible modem they installed themselves) and connects to each one
// to see its health. Mercury's read-only MONITOR can fill a heard list.
//
// Nothing on this page can transmit. Engines never get transmit permission,
// the page holds no TX program, and it sends no command that lets a modem
// answer a call. Connecting is always a click, never automatic: some modems
// accept one host program per port, so a connection here locks others out.
class HfGatewayPage : public QWidget
{
    Q_OBJECT

public:
    explicit HfGatewayPage(QWidget* parent = nullptr);
    ~HfGatewayPage() override;

private:
    void buildHeader();
    void buildEngines();
    void buildHeard();
    void buildLog();
    void rebuildEngines();
    void addEngineRow(const HfGateway::EngineConfig& config);
    void removeEngineRow(int index);
    void refreshEngineRow(int index);
    void refreshHeard();
    void addEngine();
    void removeEngine(int index);
    void saveConfig();
    void appendLog(const QString& engineLabel, const QString& text, bool warning);
    QString engineLabel(const HfGateway::HostPortEngine* engine) const;

    HfGateway::Config m_config;
    QList<HfGateway::HostPortEngine*> m_engines;

    QVBoxLayout* m_engineRows{nullptr};
    QLabel* m_noEnginesHint{nullptr};
    QComboBox* m_addProfile{nullptr};
    QLineEdit* m_addLabel{nullptr};
    QLineEdit* m_addHost{nullptr};
    QSpinBox* m_addPort{nullptr};
    QTreeWidget* m_heard{nullptr};
    QPlainTextEdit* m_log{nullptr};
};

} // namespace AetherSDR
