#pragma once

#include "PersistentDialog.h"

#include <QTimer>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace AetherSDR {

class MapView;
class AudioEngine;
class PropForecastClient;
class PskReporterClient;
class RadioModel;

// PSK Reporter reception map (View menu). Shows who is hearing our
// callsign, centered on the radio's GPS fix (falling back to the reported
// grid locator). Update cadence is fixed-interval only — PSK Reporter asks
// clients not to poll more than once per five minutes, so there is no
// manual-refresh button and the fastest non-live choice is five minutes.
class PskReporterMapDialog : public PersistentDialog {
    Q_OBJECT

public:
    // propForecast may be null; the band-conditions row is simply hidden
    // when no propagation client is available.
    explicit PskReporterMapDialog(AudioEngine* audioEngine,
                                  RadioModel* radioModel,
                                  PropForecastClient* propForecast = nullptr,
                                  QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void rebuildMarkers();
    void updateHomeFromRadio();
    void onIntervalChanged(int index);
    void onLookbackChanged(int index);
    void restartClient();
    void updateBandConditions();
    void updateConnectionIndicator();
    void scheduleBeacon();
    void stopBeacon(const QString& status);
    void updateBeaconState();
    // Stay armed and roll to the following even UTC minute. Used when the slot
    // boundary was missed, or when the DAX TX stream is still being created.
    void deferBeaconToNextSlot(const QString& reason);
    void updateBeaconDefaults();
    void setBeaconControlsEnabled(bool enabled);
    bool applyBeaconBand();
    void restoreBeaconTxFilter();

    AudioEngine*         m_audioEngine{nullptr};
    RadioModel*         m_radioModel{nullptr};
    PskReporterClient*  m_client{nullptr};
    PropForecastClient* m_propForecast{nullptr};
    MapView*            m_mapView{nullptr};
    QComboBox*          m_intervalCombo{nullptr};
    QComboBox*          m_bandCombo{nullptr};
    QComboBox*          m_modeCombo{nullptr};
    QComboBox*          m_lookbackCombo{nullptr};
    QLabel*             m_statusLabel{nullptr};
    QLabel*             m_dxLabel{nullptr};
    QLabel*             m_connLabel{nullptr};
    QCheckBox*          m_pathsCheck{nullptr};
    QTimer*             m_emptyStateTimer{nullptr};
    QTimer*             m_lookbackDebounce{nullptr};
    QTimer*             m_beaconTimer{nullptr};
    QLineEdit*          m_beaconCallsign{nullptr};
    QLineEdit*          m_beaconGrid{nullptr};
    QComboBox*          m_beaconBand{nullptr};
    QComboBox*          m_beaconPower{nullptr};
    QDoubleSpinBox*     m_beaconTone{nullptr};
    QPushButton*        m_beaconButton{nullptr};
    QLabel*             m_beaconStatus{nullptr};
    qint64              m_beaconSlotMs{0};
    qint64              m_beaconStopDeadlineMs{0};
    // How many slots this arming has rolled past waiting for the TX stream,
    // and why — the reason rides the countdown so it survives the 50 ms tick.
    int                 m_beaconDeferrals{0};
    QString             m_beaconDeferReason;
    // WSPR slots are two minutes and a frame is 111.6 s, so a late start eats
    // the headroom before it runs into the next slot. Decoders sync on the
    // signal and tolerate a small offset against the boundary; past that,
    // re-arm rather than transmit into the wrong slot.
    static constexpr qint64 kBeaconSlotMs = 2 * 60 * 1000;
    static constexpr qint64 kBeaconMaxSlotLatenessMs = 2000;
    static constexpr int    kBeaconMaxDeferrals = 2;
    int                 m_beaconPrevTxFilterLow{0};
    int                 m_beaconPrevTxFilterHigh{0};
    bool                m_beaconTxFilterSaved{false};
    bool                m_beaconArmed{false};
    bool                m_beaconTransmitting{false};
    QLabel*             m_bandCondPills[4]{};
    bool                m_started{false};
};

} // namespace AetherSDR
