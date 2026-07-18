#pragma once

#include "PersistentDialog.h"

#include <QElapsedTimer>
#include <QString>

class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;

namespace AetherSDR {

class LocationAddressResolver;
class MapView;
class RadioModel;

// Live, non-modal GPS / station-location dashboard. The radio remains the
// sole authority for every GPS and oscillator field; the dialog only derives
// presentation values such as decimal coordinates, report age, local time,
// and a transient reverse-geocoded address.
class GpsLocationDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit GpsLocationDialog(RadioModel* radioModel,
                               QWidget* parent = nullptr);

private:
    void refreshGps(bool reportArrived);
    void updateClockAndAges();
    void updateNtpServerTip();
    void requestAddress(double latitude, double longitude, bool force);
    void copyGridSquare();
    void copyAddress();
    bool currentPosition(double& latitude, double& longitude) const;

    RadioModel* m_radioModel{nullptr};
    LocationAddressResolver* m_addressResolver{nullptr};
    MapView* m_mapView{nullptr};
    QTimer* m_clockTimer{nullptr};

    QLabel* m_fixStatusLabel{nullptr};
    QLabel* m_satelliteSummaryLabel{nullptr};
    QLabel* m_referenceSummaryLabel{nullptr};
    QLabel* m_freshnessLabel{nullptr};

    QLabel* m_gridLabel{nullptr};
    QLabel* m_latitudeLabel{nullptr};
    QLabel* m_longitudeLabel{nullptr};
    QLabel* m_nativeCoordinatesLabel{nullptr};
    QLabel* m_altitudeLabel{nullptr};
    QLabel* m_speedLabel{nullptr};
    QLabel* m_courseLabel{nullptr};
    QLabel* m_addressLabel{nullptr};
    QPushButton* m_copyGridSquareButton{nullptr};
    QPushButton* m_copyAddressButton{nullptr};
    QPushButton* m_refreshAddressButton{nullptr};

    QLabel* m_trackedLabel{nullptr};
    QLabel* m_visibleLabel{nullptr};
    QLabel* m_trackingRatioLabel{nullptr};
    QProgressBar* m_satelliteProgress{nullptr};
    QLabel* m_frequencyErrorLabel{nullptr};
    QLabel* m_referenceSettingLabel{nullptr};
    QLabel* m_referenceActualLabel{nullptr};
    QLabel* m_referenceLockLabel{nullptr};
    QLabel* m_lockDurationLabel{nullptr};

    QLabel* m_utcTimeLabel{nullptr};
    QLabel* m_localTimeLabel{nullptr};
    QLabel* m_timeZoneLabel{nullptr};
    QLabel* m_radioGpsTimeLabel{nullptr};
    QLabel* m_clockAgreementLabel{nullptr};
    QLabel* m_ntpServerTipLabel{nullptr};

    QElapsedTimer m_reportAge;
    qint64 m_lockBeganMs{0};
    bool m_wasGpsLocked{false};
    bool m_hasPosition{false};
    double m_latitude{0.0};
    double m_longitude{0.0};
    bool m_hasAddressQueryPosition{false};
    double m_addressQueryLatitude{0.0};
    double m_addressQueryLongitude{0.0};
    QString m_resolvedAddress;
};

} // namespace AetherSDR
