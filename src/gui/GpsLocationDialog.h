#pragma once

#include "CanonWindow.h"

#include "core/GpsdSkyClient.h"

#include <QElapsedTimer>
#include <QString>

class QLabel;
class QCheckBox;
class QFrame;
class QGroupBox;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTimer;

namespace AetherSDR {

class LocationAddressResolver;
class GpsSkyPlot;
class AetherGpsTools;
class MapView;
class RadioModel;

// AetherGPS: the live, non-modal GPS / station-location dashboard, a canon
// workspace window. The radio remains the sole authority for every GPS and
// oscillator field; the window only derives presentation values such as
// decimal coordinates, report age, local time, and a transient
// reverse-geocoded address.
class GpsLocationDialog : public CanonWindow {
    Q_OBJECT

public:
    explicit GpsLocationDialog(RadioModel* radioModel,
                               QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void refreshGps(bool reportArrived);
    // Starts or stops the sky view's gpsd session (the AetherGPS waveform on
    // the radio): it reads only while this window shows and the radio is
    // reachable on its LAN or through the tailnet shim.
    void updateSkySource();
    // A SKY report, possibly listing no satellites.
    void showSky(const QList<GpsSatellite>& satellites);
    // Empties the plot and table and says why.
    void clearSky(const QString& status);
    void showSkyState(GpsdSkyClient::State state, const QString& detail);
    void showDetail(const GpsdDetail& detail);
    // The bottom bar: "Install AetherGPS on the radio" mirrors whether the
    // radio lists the waveform and installs or removes it; the Sky View
    // shows only while it is installed.
    void updateGpsTools();
    void onGpsToolsToggled(bool install);
    void updateClockAndAges();
    void updateNtpServerTip();
    void updateGpsTimeControls();
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
    QFrame* m_satelliteMetricCard{nullptr};
    QFrame* m_referenceMetricCard{nullptr};

    QLabel* m_gridLabel{nullptr};
    QLabel* m_sourceLabel{nullptr};
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
    QGroupBox* m_satelliteGroup{nullptr};
    QLabel* m_satelliteApiNote{nullptr};

    GpsdSkyClient* m_skyClient{nullptr};
    QGroupBox* m_skyGroup{nullptr};
    GpsSkyPlot* m_skyPlot{nullptr};
    QTableWidget* m_skyTable{nullptr};
    QLabel* m_skyStatusLabel{nullptr};
    bool m_skyHasData{false};

    QLabel* m_fixModeLabel{nullptr};
    QLabel* m_dopLabel{nullptr};
    QLabel* m_dopTimeLabel{nullptr};
    QLabel* m_errorHorizontalLabel{nullptr};
    QLabel* m_errorVerticalLabel{nullptr};
    QLabel* m_errorTimeLabel{nullptr};
    QLabel* m_ppsOffsetLabel{nullptr};
    QLabel* m_receiverLabel{nullptr};
    QLabel* m_ppsSourceLabel{nullptr};
    QLabel* m_gpsdVersionLabel{nullptr};

    QFrame* m_gpsToolsBar{nullptr};
    QCheckBox* m_gpsToolsCheck{nullptr};
    QLabel* m_gpsToolsLabel{nullptr};
    AetherGpsTools* m_gpsTools{nullptr};
    bool m_gpsToolsInstalled{false};
    int m_installPercent{-1};   // the install's last progress, -1 before any

    QLabel* m_utcTimeLabel{nullptr};
    QLabel* m_localTimeLabel{nullptr};
    QLabel* m_timeZoneLabel{nullptr};
    QLabel* m_radioGpsTimeLabel{nullptr};
    QLabel* m_clockAgreementLabel{nullptr};
    QLabel* m_timeSourceNote{nullptr};
    QLabel* m_ntpServerTipLabel{nullptr};
    QGroupBox* m_gpsTimeControlGroup{nullptr};
    QCheckBox* m_ntpEnabledCheck{nullptr};
    QLineEdit* m_ntpServerEdit{nullptr};
    QPushButton* m_applyNtpServerButton{nullptr};
    QCheckBox* m_gpsTimeCorrectionCheck{nullptr};
    QPushButton* m_ntpSyncButton{nullptr};
    QLabel* m_ntpSyncStatusLabel{nullptr};

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
