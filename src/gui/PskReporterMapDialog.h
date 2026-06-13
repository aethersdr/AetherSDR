#pragma once

#include "PersistentDialog.h"

#include <QTimer>

class QCheckBox;
class QComboBox;
class QLabel;

namespace AetherSDR {

class MapView;
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
    explicit PskReporterMapDialog(RadioModel* radioModel,
                                  QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void rebuildMarkers();
    void updateHomeFromRadio();
    void onIntervalChanged(int index);
    void restartClient();

    RadioModel*        m_radioModel{nullptr};
    PskReporterClient* m_client{nullptr};
    MapView*           m_mapView{nullptr};
    QComboBox*         m_intervalCombo{nullptr};
    QComboBox*         m_bandCombo{nullptr};
    QComboBox*         m_modeCombo{nullptr};
    QLabel*            m_statusLabel{nullptr};
    QLabel*            m_dxLabel{nullptr};
    QCheckBox*         m_pathsCheck{nullptr};
    QTimer*            m_emptyStateTimer{nullptr};
    bool               m_started{false};
};

} // namespace AetherSDR
