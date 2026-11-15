#pragma once

#include "models/DStarModel.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QToolButton;

namespace AetherSDR {

class RadioModel;

class DStarModemPage : public QWidget
{
    Q_OBJECT

public:
    explicit DStarModemPage(RadioModel* radio, QWidget* parent = nullptr);

private:
    void buildHeader();
    QWidget* buildConfigurationPanel();
    QWidget* buildTrafficPanel();
    void buildFooter();

    void refreshAll();
    void refreshService();
    void refreshConfiguration();
    void refreshSerialDevices();
    void refreshTraffic();
    void refreshRouteSummary();
    void refreshMessageCounter();
    void refreshDestinationControl();
    void setError(const QString& message);

    DStarRouteRequest routeRequestFromWidgets() const;
    bool configurationFromWidgets(bool useRawRoute,
                                  DStarConfiguration* config,
                                  QString* error = nullptr) const;
    bool saveConfiguration(bool useRawRoute);
    void setTxMessage();
    void startStopService();
    void browseExecutable();

    RadioModel* m_radio{nullptr};
    bool m_updating{false};

    QLabel* m_serviceDot{nullptr};
    QLabel* m_serviceState{nullptr};
    QLabel* m_sliceState{nullptr};
    QPushButton* m_startStopButton{nullptr};
    QLabel* m_errorLabel{nullptr};

    QLineEdit* m_myCallEdit{nullptr};
    QLineEdit* m_suffixEdit{nullptr};
    QComboBox* m_fromCombo{nullptr};
    QComboBox* m_toCombo{nullptr};
    QLabel* m_accessRepeaterLabel{nullptr};
    QWidget* m_accessRepeaterRow{nullptr};
    QLineEdit* m_accessRepeaterEdit{nullptr};
    QComboBox* m_accessRepeaterModule{nullptr};
    QLabel* m_destinationLabel{nullptr};
    QWidget* m_destinationRow{nullptr};
    QLineEdit* m_destinationEdit{nullptr};
    QComboBox* m_destinationModule{nullptr};
    QLabel* m_routeSummary{nullptr};
    QToolButton* m_advancedButton{nullptr};
    QWidget* m_advancedPanel{nullptr};
    QLineEdit* m_urCallEdit{nullptr};
    QLineEdit* m_rpt1Edit{nullptr};
    QLineEdit* m_rpt2Edit{nullptr};
    QLineEdit* m_executableEdit{nullptr};
    QPushButton* m_browseButton{nullptr};
    QComboBox* m_deviceCombo{nullptr};
    QPushButton* m_refreshDevicesButton{nullptr};
    QCheckBox* m_autoStartCheck{nullptr};

    QComboBox* m_stationFilter{nullptr};
    QLineEdit* m_searchEdit{nullptr};
    QToolButton* m_clearTrafficButton{nullptr};
    QListWidget* m_trafficList{nullptr};
    QLineEdit* m_messageEdit{nullptr};
    QLabel* m_messageCounter{nullptr};
    QPushButton* m_setMessageButton{nullptr};
    QLabel* m_activeMessage{nullptr};

    QLabel* m_footerState{nullptr};
    QLabel* m_footerRoute{nullptr};
    QLabel* m_footerActivity{nullptr};
};

} // namespace AetherSDR
