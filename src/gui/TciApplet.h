#pragma once

#include <QWidget>

class QPushButton;
class QLabel;
class QLineEdit;

namespace AetherSDR {

class RadioModel;
class TciServer;
class MeterSlider;

// TCI Applet — TCI WebSocket server control panel.
// Mirrors DaxApplet framework: per-channel meters + gain sliders for 8 RX
// channels plus 1 TX channel, a port QLineEdit, and an Enable toggle.
//
// Entire implementation is gated by HAVE_WEBSOCKETS. When the build does not
// include WebSocket support, the applet compiles to an empty widget.
class TciApplet : public QWidget {
    Q_OBJECT

public:
    static constexpr int kChannels = 8;

    explicit TciApplet(QWidget* parent = nullptr);

    void setRadioModel(RadioModel* model);
    void setTciServer(TciServer* tci);

    // Bound the operator-facing RX row count to the radio's slice capacity
    // (mirrors DaxApplet). Rows above n are HIDDEN, not destroyed — kChannels
    // stays the allocation. Fed by MainWindow from RadioModel::maxSlices().
    // (#4854 review)
    void setMaxDaxChannels(int n);

    // Sync Enable button state (called by MainWindow on autostart)
    void setTciEnabled(bool on);
    void setTciRxLevel(int channel, float rms);  // channel 1-4
    void setTciTxLevel(float rms);

signals:
    void tciToggled(bool on);
    void tciRxGainChanged(int channel, float gain);  // 1-4, 0.0–1.0
    void tciTxGainChanged(float gain);
    // 0=Clip, 1=NaNGuard, 2=Measure — selected via right-click on TX slider.
    void tciTxOverflowModeChanged(int mode);

private:
#ifdef HAVE_WEBSOCKETS
    void buildUI();
    void updateTciStatus();

    RadioModel* m_model{nullptr};
    TciServer*  m_tciServer{nullptr};
    int m_maxDaxChannels{kChannels};  // radio slice capacity (FlexLib table)

    QPushButton* m_tciEnable{nullptr};
    QLineEdit*   m_tciPort{nullptr};
    QLabel*      m_tciStatus{nullptr};

    QWidget*     m_rxRow[kChannels]{};  // per-channel row container — hidden to gate to maxSlices()
    MeterSlider* m_rxMeter[kChannels]{};
    QLabel*      m_rxStatus[kChannels]{};
    MeterSlider* m_txMeter{nullptr};
    QLabel*      m_txStatus{nullptr};
#endif
};

} // namespace AetherSDR
