#pragma once

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace AetherSDR {

class Data2gReceiveModel;
struct Data2gHeardBurst;

// AetherModem's "HF Data" tab (RFC #7159, Stage 1): a receive-only monitor
// for Data2G. It lists every burst heard on the modem's receive audio and has
// no control that can transmit.
//
// The page owns no audio. The modem window feeds the model from its own
// receive tap, so the monitor can only run while the modem is enabled; the
// window reports that through setModemEnabled().
class Data2gMonitorPage : public QWidget
{
    Q_OBJECT

public:
    explicit Data2gMonitorPage(Data2gReceiveModel* model, QWidget* parent = nullptr);

    // Whether the modem's receive tap is running. While it is not, the
    // Monitor switch is dimmed and the page says why.
    void setModemEnabled(bool enabled);

private:
    void buildHeader();
    void buildLog();
    void buildFooter();
    void appendBurst(const Data2gHeardBurst& burst);
    void refreshState();
    void refreshStats();

    QPointer<Data2gReceiveModel> m_model;
    bool m_modemEnabled{false};

    QLabel* m_stateDot{nullptr};
    QLabel* m_stateLabel{nullptr};
    QCheckBox* m_monitorCheck{nullptr};
    QPushButton* m_clearButton{nullptr};
    QPlainTextEdit* m_log{nullptr};
    QLabel* m_emptyHint{nullptr};
    QLabel* m_burstsValue{nullptr};
    QLabel* m_overflowsValue{nullptr};
    QLabel* m_faultsValue{nullptr};
};

} // namespace AetherSDR
