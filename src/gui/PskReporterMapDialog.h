#pragma once

#include "PersistentDialog.h"

#include <QTimer>

class QCheckBox;
class QComboBox;
class QLabel;

namespace AetherSDR {

class MapView;
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
    explicit PskReporterMapDialog(RadioModel* radioModel,
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
    QLabel*             m_bandCondPills[4]{};
    bool                m_started{false};
};

} // namespace AetherSDR
