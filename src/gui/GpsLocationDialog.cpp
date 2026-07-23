#include "GpsLocationDialog.h"

#include "core/LocationAddressResolver.h"
#include "core/ThemeManager.h"
#include "core/aprs/AprsPacket.h"
#include "map/MapView.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace AetherSDR {

namespace {

constexpr double kAddressMovementThresholdDegrees = 0.001;

QString displayValue(const QString& value)
{
    return value.trimmed().isEmpty() ? QStringLiteral("—") : value.trimmed();
}

bool statusIsLocked(QString status)
{
    status = status.trimmed().toLower();
    if (!status.contains(QLatin1String("lock"))) {
        return false;
    }
    // Reject every negative phrasing that still contains "lock" — the radio's
    // exact status vocabulary varies by firmware, so match intent rather than a
    // single literal: "unlocked", "no lock", "not locked", "lock lost",
    // "loss of lock", etc. all mean not-locked.
    for (const char* negative : {"unlock", "no lock", "not lock", "lost", "loss"}) {
        if (status.contains(QLatin1String(negative))) {
            return false;
        }
    }
    return true;
}

QString referenceName(QString value)
{
    value = value.trimmed().toLower();
    if (value == QLatin1String("gpsdo")) {
        return QStringLiteral("GPSDO");
    }
    if (value == QLatin1String("tcxo")) {
        return QStringLiteral("TCXO");
    }
    if (value == QLatin1String("ext") || value == QLatin1String("external")) {
        return QStringLiteral("External 10 MHz");
    }
    if (value == QLatin1String("auto")) {
        return QStringLiteral("Auto");
    }
    return displayValue(value.toUpper());
}

QString durationText(qint64 seconds)
{
    if (seconds < 60) {
        return GpsLocationDialog::tr("%1 s").arg(seconds);
    }
    if (seconds < 3600) {
        return GpsLocationDialog::tr("%1 min %2 s")
            .arg(seconds / 60)
            .arg(seconds % 60);
    }
    return GpsLocationDialog::tr("%1 h %2 min")
        .arg(seconds / 3600)
        .arg((seconds % 3600) / 60);
}

void configureReadout(QLabel* label, const QString& objectName,
                      const QString& accessibleName)
{
    label->setObjectName(objectName);
    label->setProperty("gpsRole", QStringLiteral("value"));
    label->setAccessibleName(accessibleName);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse
                                   | Qt::TextSelectableByKeyboard);
}

QFrame* makeMetricCard(const QString& title, const QString& objectName,
                       const QString& accessibleName, QLabel*& value,
                       QWidget* parent)
{
    auto* card = new QFrame(parent);
    card->setProperty("gpsMetricCard", true);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(12, 9, 12, 9);
    layout->setSpacing(3);

    auto* titleLabel = new QLabel(title, card);
    titleLabel->setProperty("gpsRole", QStringLiteral("metricTitle"));
    layout->addWidget(titleLabel);

    value = new QLabel(QStringLiteral("—"), card);
    value->setObjectName(objectName);
    value->setProperty("gpsRole", QStringLiteral("metricValue"));
    value->setAccessibleName(accessibleName);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse
                                   | Qt::TextSelectableByKeyboard);
    layout->addWidget(value);
    return card;
}

QLabel* makeSectionTitle(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setProperty("gpsRole", QStringLiteral("sectionTitle"));
    label->setAccessibleName(text);
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    return label;
}

void addReadoutRow(QGridLayout* layout, int row, const QString& title,
                   QLabel* value)
{
    auto* titleLabel = new QLabel(title);
    titleLabel->setProperty("gpsRole", QStringLiteral("fieldTitle"));
    layout->addWidget(titleLabel, row, 0, Qt::AlignTop);
    layout->addWidget(value, row, 1, Qt::AlignTop);
}

QPushButton* makeActionButton(const QString& text, const QString& objectName,
                              const QString& accessibleDescription,
                              QWidget* parent)
{
    auto* button = new QPushButton(text, parent);
    button->setObjectName(objectName);
    button->setAutoDefault(false);
    button->setDefault(false);
    button->setAccessibleName(text);
    button->setAccessibleDescription(accessibleDescription);
    return button;
}

} // namespace

GpsLocationDialog::GpsLocationDialog(RadioModel* radioModel, QWidget* parent)
    : PersistentDialog(tr("GPS & Station Location"),
                       QStringLiteral("GpsLocationDialogGeometry"), parent)
    , m_radioModel(radioModel)
    , m_addressResolver(new LocationAddressResolver(this))
{
    setObjectName(QStringLiteral("gpsLocationDialog"));
    setMinimumSize(860, 600);
    resize(1080, 720);

    ThemeManager::instance().applyStyleSheet(this, QStringLiteral(
        "QDialog { background: {{color.background.0}}; color: {{color.text.primary}}; }"
        "QScrollArea, QScrollArea > QWidget > QWidget { background: {{color.background.0}}; border: none; }"
        "QWidget#gpsAddressColumn { background: transparent; }"
        "QLabel { background: transparent; }"
        "QFrame[gpsHeader='true'] { background: {{color.background.1}}; border: 1px solid {{color.border.strong}}; border-radius: 10px; }"
        "QFrame[gpsMetricCard='true'] { background: {{color.background.0}}; border: 1px solid {{color.border.subtle}}; border-radius: 7px; }"
        "QGroupBox { color: {{color.text.primary}}; background: {{color.background.1}}; border: 1px solid {{color.border.strong}}; border-radius: 10px; }"
        "QLabel[gpsRole='sectionTitle'] { color: {{color.accent}}; font-size: 14px; font-weight: 700; padding-bottom: 4px; }"
        "QLabel[gpsRole='metricTitle'], QLabel[gpsRole='fieldTitle'] { color: {{color.text.secondary}}; font-size: 11px; }"
        "QLabel[gpsRole='metricValue'] { color: {{color.text.primary}}; font-size: 18px; font-weight: 700; }"
        "QLabel[gpsRole='value'] { color: {{color.text.primary}}; font-size: 13px; font-weight: 600; }"
        "QLabel[gpsRole='muted'] { color: {{color.text.secondary}}; font-size: 11px; }"
        "QLabel[gpsRole='tip'] { color: {{color.text.primary}}; background: {{color.background.2}}; border: 1px solid {{color.border.subtle}}; border-radius: 6px; padding: 8px; font-size: 12px; }"
        "QPushButton { color: {{color.text.primary}}; background: {{color.background.2}}; border: 1px solid {{color.border.strong}}; border-radius: 5px; padding: 5px 10px; }"
        "QPushButton:hover, QPushButton:focus { border-color: {{color.border.accent}}; }"
        "QPushButton:pressed { background: {{color.background.3}}; }"
        "QProgressBar { color: {{color.text.primary}}; selection-color: #ffffff; background: {{color.background.0}}; border: 1px solid {{color.border.strong}}; border-radius: 5px; text-align: center; min-height: 22px; }"
        "QProgressBar::chunk { background: #17663f; border-radius: 4px; }"));

    auto* outer = new QVBoxLayout(bodyWidget());
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(8);

    auto* scroll = new QScrollArea(bodyWidget());
    scroll->setObjectName(QStringLiteral("gpsLocationScrollArea"));
    scroll->setWidgetResizable(true);
    outer->addWidget(scroll);

    auto* content = new QWidget(scroll);
    auto* root = new QGridLayout(content);
    root->setContentsMargins(4, 4, 4, 4);
    root->setHorizontalSpacing(10);
    root->setVerticalSpacing(10);
    root->setColumnStretch(0, 5);
    root->setColumnStretch(1, 6);
    scroll->setWidget(content);
    QTimer::singleShot(0, scroll, [scroll] {
        scroll->verticalScrollBar()->setValue(0);
    });

    auto* header = new QFrame(content);
    header->setProperty("gpsHeader", true);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(9, 9, 9, 9);
    headerLayout->setSpacing(8);
    headerLayout->addWidget(makeMetricCard(
        tr("GPS FIX"), QStringLiteral("gpsFixStatus"), tr("GPS fix status"),
        m_fixStatusLabel, header));
    headerLayout->addWidget(makeMetricCard(
        tr("SATELLITES"), QStringLiteral("gpsSatelliteSummary"),
        tr("GPS satellites tracked and visible"), m_satelliteSummaryLabel, header));
    headerLayout->addWidget(makeMetricCard(
        tr("10 MHz REFERENCE"), QStringLiteral("gpsReferenceSummary"),
        tr("Radio frequency reference status"), m_referenceSummaryLabel, header));
    headerLayout->addWidget(makeMetricCard(
        tr("TELEMETRY"), QStringLiteral("gpsReportFreshness"),
        tr("Age of the latest GPS report"), m_freshnessLabel, header));
    root->addWidget(header, 0, 0, 1, 2);

    auto* locationGroup = new QGroupBox(content);
    locationGroup->setAccessibleName(tr("Current Location"));
    auto* locationLayout = new QGridLayout(locationGroup);
    locationLayout->setContentsMargins(14, 12, 14, 12);
    locationLayout->setHorizontalSpacing(14);
    locationLayout->setVerticalSpacing(7);
    locationLayout->setColumnStretch(1, 1);
    locationLayout->addWidget(
        makeSectionTitle(tr("Current Location"), locationGroup), 0, 0, 1, 2);

    m_gridLabel = new QLabel;
    configureReadout(m_gridLabel, QStringLiteral("gpsGrid"), tr("Maidenhead grid square"));
    addReadoutRow(locationLayout, 1, tr("Grid square"), m_gridLabel);
    m_latitudeLabel = new QLabel;
    configureReadout(m_latitudeLabel, QStringLiteral("gpsLatitude"), tr("GPS latitude"));
    addReadoutRow(locationLayout, 2, tr("Latitude"), m_latitudeLabel);
    m_longitudeLabel = new QLabel;
    configureReadout(m_longitudeLabel, QStringLiteral("gpsLongitude"), tr("GPS longitude"));
    addReadoutRow(locationLayout, 3, tr("Longitude"), m_longitudeLabel);
    m_nativeCoordinatesLabel = new QLabel;
    configureReadout(m_nativeCoordinatesLabel, QStringLiteral("gpsNativeCoordinates"),
                     tr("Coordinates as reported by the radio"));
    m_nativeCoordinatesLabel->setWordWrap(true);
    addReadoutRow(locationLayout, 4, tr("Radio format"), m_nativeCoordinatesLabel);
    m_altitudeLabel = new QLabel;
    configureReadout(m_altitudeLabel, QStringLiteral("gpsAltitude"), tr("GPS altitude"));
    addReadoutRow(locationLayout, 5, tr("Altitude"), m_altitudeLabel);
    m_speedLabel = new QLabel;
    configureReadout(m_speedLabel, QStringLiteral("gpsSpeed"), tr("GPS speed"));
    addReadoutRow(locationLayout, 6, tr("Speed"), m_speedLabel);
    m_courseLabel = new QLabel;
    configureReadout(m_courseLabel, QStringLiteral("gpsCourse"), tr("GPS course over ground"));
    addReadoutRow(locationLayout, 7, tr("Course"), m_courseLabel);

    auto* addressTitle = new QLabel(tr("Nearest mapped address"));
    addressTitle->setProperty("gpsRole", QStringLiteral("fieldTitle"));
    locationLayout->addWidget(addressTitle, 8, 0, Qt::AlignTop);
    auto* addressColumn = new QWidget(locationGroup);
    addressColumn->setObjectName(QStringLiteral("gpsAddressColumn"));
    auto* addressLayout = new QVBoxLayout(addressColumn);
    addressLayout->setContentsMargins(0, 0, 0, 0);
    addressLayout->setSpacing(4);
    m_addressLabel = new QLabel(tr("Waiting for a valid GPS fix"), addressColumn);
    configureReadout(m_addressLabel, QStringLiteral("gpsMappedAddress"),
                     tr("Nearest mapped address"));
    m_addressLabel->setWordWrap(true);
    addressLayout->addWidget(m_addressLabel);
    auto* attribution = new QLabel(
        tr("Address data © OpenStreetMap contributors · online lookup"), addressColumn);
    attribution->setProperty("gpsRole", QStringLiteral("muted"));
    attribution->setAccessibleName(tr("OpenStreetMap address attribution"));
    addressLayout->addWidget(attribution);
    locationLayout->addWidget(addressColumn, 8, 1, Qt::AlignTop);

    auto* actions = new QHBoxLayout;
    actions->setContentsMargins(0, 6, 0, 0);
    actions->setSpacing(8);
    m_copyGridSquareButton = makeActionButton(
        tr("Copy gridsquare"), QStringLiteral("gpsCopyGridSquare"),
        tr("Copy the Maidenhead grid square to the clipboard"), locationGroup);
    connect(m_copyGridSquareButton, &QPushButton::clicked,
            this, &GpsLocationDialog::copyGridSquare);
    actions->addWidget(m_copyGridSquareButton);
    m_copyAddressButton = makeActionButton(
        tr("Copy address"), QStringLiteral("gpsCopyAddress"),
        tr("Copy the nearest mapped address to the clipboard"), locationGroup);
    m_copyAddressButton->setEnabled(false);
    connect(m_copyAddressButton, &QPushButton::clicked,
            this, &GpsLocationDialog::copyAddress);
    actions->addWidget(m_copyAddressButton);
    m_refreshAddressButton = makeActionButton(
        tr("Refresh address"), QStringLiteral("gpsRefreshAddress"),
        tr("Send the current GPS coordinates for an online address lookup"), locationGroup);
    connect(m_refreshAddressButton, &QPushButton::clicked, this, [this] {
        if (m_hasPosition) {
            requestAddress(m_latitude, m_longitude, true);
        }
    });
    actions->addWidget(m_refreshAddressButton);
    actions->addStretch();
    locationLayout->addLayout(actions, 9, 0, 1, 2, Qt::AlignTop);
    locationLayout->setRowStretch(10, 1);
    root->addWidget(locationGroup, 1, 0);

    auto* mapGroup = new QGroupBox(content);
    mapGroup->setAccessibleName(tr("Station Map"));
    auto* mapLayout = new QVBoxLayout(mapGroup);
    mapLayout->setContentsMargins(10, 12, 10, 10);
    mapLayout->setSpacing(7);
    mapLayout->addWidget(makeSectionTitle(tr("Station Map"), mapGroup));
    m_mapView = new MapView(mapGroup);
    m_mapView->setObjectName(QStringLiteral("gpsLocationMap"));
    m_mapView->setAccessibleName(tr("Map of the radio GPS location"));
    m_mapView->setAccessibleDescription(
        tr("Use arrow keys to pan, plus and minus to zoom, and Home to recenter"));
    m_mapView->setMinimumSize(380, 300);
    m_mapView->setMaximumHeight(360);
    m_mapView->setHomeSpanDegrees(0.15);
    m_mapView->setPathsVisible(false);
    mapLayout->addWidget(m_mapView);
    // Geometry restore and the two-column layout can resize QGeoView after
    // its first show. Recenter once the event loop has the final viewport so
    // the initial camera does not leave an uncovered strip at the right edge.
    QTimer::singleShot(0, m_mapView, &MapView::resetToHome);
    root->addWidget(mapGroup, 1, 1);

    auto* satelliteGroup = new QGroupBox(content);
    satelliteGroup->setAccessibleName(
        tr("Satellite Reception and Frequency Reference"));
    auto* satelliteLayout = new QGridLayout(satelliteGroup);
    satelliteLayout->setContentsMargins(14, 12, 14, 12);
    satelliteLayout->setHorizontalSpacing(18);
    satelliteLayout->setVerticalSpacing(7);
    satelliteLayout->setColumnStretch(1, 1);
    satelliteLayout->addWidget(makeSectionTitle(
        tr("Satellite Reception and Frequency Reference"), satelliteGroup),
        0, 0, 1, 2);

    m_satelliteProgress = new QProgressBar(satelliteGroup);
    m_satelliteProgress->setObjectName(QStringLiteral("gpsSatelliteProgress"));
    m_satelliteProgress->setAccessibleName(tr("Satellite tracking ratio"));
    m_satelliteProgress->setRange(0, 100);
    satelliteLayout->addWidget(m_satelliteProgress, 1, 0, 1, 2);
    m_trackedLabel = new QLabel;
    configureReadout(m_trackedLabel, QStringLiteral("gpsSatellitesTracked"),
                     tr("Satellites tracked"));
    addReadoutRow(satelliteLayout, 2, tr("Tracked"), m_trackedLabel);
    m_visibleLabel = new QLabel;
    configureReadout(m_visibleLabel, QStringLiteral("gpsSatellitesVisible"),
                     tr("Satellites visible"));
    addReadoutRow(satelliteLayout, 3, tr("Visible"), m_visibleLabel);
    m_trackingRatioLabel = new QLabel;
    configureReadout(m_trackingRatioLabel, QStringLiteral("gpsTrackingRatio"),
                     tr("Percentage of visible satellites tracked"));
    addReadoutRow(satelliteLayout, 4, tr("Tracking ratio"), m_trackingRatioLabel);
    m_frequencyErrorLabel = new QLabel;
    configureReadout(m_frequencyErrorLabel, QStringLiteral("gpsFrequencyError"),
                     tr("GPS frequency error reported by the radio"));
    addReadoutRow(satelliteLayout, 5, tr("Frequency error"), m_frequencyErrorLabel);
    m_referenceSettingLabel = new QLabel;
    configureReadout(m_referenceSettingLabel, QStringLiteral("gpsReferenceSetting"),
                     tr("Configured radio reference source"));
    addReadoutRow(satelliteLayout, 6, tr("Reference setting"), m_referenceSettingLabel);
    m_referenceActualLabel = new QLabel;
    configureReadout(m_referenceActualLabel, QStringLiteral("gpsReferenceActual"),
                     tr("Actual radio reference source"));
    addReadoutRow(satelliteLayout, 7, tr("Reference actual"), m_referenceActualLabel);
    m_referenceLockLabel = new QLabel;
    configureReadout(m_referenceLockLabel, QStringLiteral("gpsReferenceLock"),
                     tr("Frequency reference lock"));
    addReadoutRow(satelliteLayout, 8, tr("Reference lock"), m_referenceLockLabel);
    m_lockDurationLabel = new QLabel;
    configureReadout(m_lockDurationLabel, QStringLiteral("gpsLockDuration"),
                     tr("Duration of the current GPS lock while this window is open"));
    addReadoutRow(satelliteLayout, 9, tr("GPS lock duration"), m_lockDurationLabel);
    auto* apiNote = new QLabel(
        tr("AetherSDR reports aggregate tracked/visible counts, but not individual "
           "satellite IDs, azimuth, elevation, or signal strength. Orbital positions "
           "are therefore intentionally not estimated."), satelliteGroup);
    apiNote->setObjectName(QStringLiteral("gpsSatelliteApiNote"));
    apiNote->setProperty("gpsRole", QStringLiteral("muted"));
    apiNote->setWordWrap(true);
    apiNote->setAccessibleName(tr("Satellite API limitation"));
    satelliteLayout->addWidget(apiNote, 10, 0, 1, 2);
    satelliteLayout->setRowStretch(11, 1);
    root->addWidget(satelliteGroup, 2, 0);

    auto* timeGroup = new QGroupBox(content);
    timeGroup->setAccessibleName(tr("Satellite Time"));
    auto* timeLayout = new QGridLayout(timeGroup);
    timeLayout->setContentsMargins(14, 12, 14, 12);
    timeLayout->setHorizontalSpacing(14);
    timeLayout->setVerticalSpacing(9);
    timeLayout->setColumnStretch(1, 1);
    timeLayout->addWidget(makeSectionTitle(tr("Satellite Time"), timeGroup), 0, 0, 1, 2);
    m_utcTimeLabel = new QLabel;
    configureReadout(m_utcTimeLabel, QStringLiteral("gpsUtcTime"), tr("Current UTC time"));
    addReadoutRow(timeLayout, 1, tr("UTC / GMT"), m_utcTimeLabel);
    m_localTimeLabel = new QLabel;
    configureReadout(m_localTimeLabel, QStringLiteral("gpsLocalTime"), tr("Current local time"));
    addReadoutRow(timeLayout, 2, tr("Local"), m_localTimeLabel);
    m_timeZoneLabel = new QLabel;
    configureReadout(m_timeZoneLabel, QStringLiteral("gpsTimeZone"), tr("Local time zone"));
    addReadoutRow(timeLayout, 3, tr("Time zone"), m_timeZoneLabel);
    m_radioGpsTimeLabel = new QLabel;
    configureReadout(m_radioGpsTimeLabel, QStringLiteral("gpsRadioTime"),
                     tr("UTC time reported by the radio GPS"));
    addReadoutRow(timeLayout, 4, tr("Radio GPS UTC"), m_radioGpsTimeLabel);
    m_clockAgreementLabel = new QLabel;
    configureReadout(m_clockAgreementLabel, QStringLiteral("gpsClockAgreement"),
                     tr("Agreement between radio GPS time and computer UTC"));
    m_clockAgreementLabel->setWordWrap(true);
    addReadoutRow(timeLayout, 5, tr("Clock agreement"), m_clockAgreementLabel);
    auto* timeNote = new QLabel(
        tr("The radio GPS feed supplies UTC time-of-day but no date. UTC/local dates "
           "above therefore come from the computer clock. Use Clock agreement as a "
           "coarse guide when interpreting FT8 time differential (DT), and keep the "
           "computer synchronized with NTP for precise operation."), timeGroup);
    timeNote->setProperty("gpsRole", QStringLiteral("muted"));
    timeNote->setWordWrap(true);
    timeNote->setAccessibleName(tr("GPS date source explanation"));
    timeLayout->addWidget(timeNote, 6, 0, 1, 2);
    m_ntpServerTipLabel = new QLabel(timeGroup);
    m_ntpServerTipLabel->setObjectName(QStringLiteral("gpsNtpServerTip"));
    m_ntpServerTipLabel->setProperty("gpsRole", QStringLiteral("tip"));
    m_ntpServerTipLabel->setWordWrap(true);
    m_ntpServerTipLabel->setTextInteractionFlags(
        Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_ntpServerTipLabel->setAccessibleName(tr("Radio NTP server tip"));
    m_ntpServerTipLabel->setAccessibleDescription(
        tr("Local FLEX-8000 NTP server address; use it only on a trusted network"));
    timeLayout->addWidget(m_ntpServerTipLabel, 7, 0, 1, 2);
    timeLayout->setRowStretch(8, 1);
    root->addWidget(timeGroup, 2, 1);

    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(0, 0, 0, 0);
    footer->addStretch();
    auto* closeButton = makeActionButton(
        tr("Close"), QStringLiteral("gpsCloseButton"),
        tr("Close the GPS and station location window"), bodyWidget());
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    footer->addWidget(closeButton);
    outer->addLayout(footer);

    connect(m_addressResolver, &LocationAddressResolver::resolved,
            this, [this](double latitude, double longitude, const QString& address) {
                if (!m_hasPosition
                    || std::abs(latitude - m_latitude) > kAddressMovementThresholdDegrees
                    || std::abs(longitude - m_longitude) > kAddressMovementThresholdDegrees) {
                    return;
                }
                m_resolvedAddress = address;
                m_addressLabel->setText(address);
                m_copyAddressButton->setEnabled(!address.isEmpty());
                m_refreshAddressButton->setEnabled(true);
            });
    connect(m_addressResolver, &LocationAddressResolver::failed,
            this, [this](double latitude, double longitude, const QString& reason) {
                if (!m_hasPosition
                    || std::abs(latitude - m_latitude) > kAddressMovementThresholdDegrees
                    || std::abs(longitude - m_longitude) > kAddressMovementThresholdDegrees) {
                    return;
                }
                m_addressLabel->setText(reason);
                m_copyAddressButton->setEnabled(false);
                m_refreshAddressButton->setEnabled(true);
            });

    if (m_radioModel != nullptr) {
        connect(m_radioModel, &RadioModel::gpsStatusChanged, this,
                [this] { refreshGps(true); });
        connect(m_radioModel, &RadioModel::oscillatorChanged, this,
                [this] { refreshGps(false); });
        connect(m_radioModel, &RadioModel::infoChanged,
                this, &GpsLocationDialog::updateNtpServerTip);
    }

    m_clockTimer = new QTimer(this);
    m_clockTimer->setInterval(1000);
    connect(m_clockTimer, &QTimer::timeout,
            this, &GpsLocationDialog::updateClockAndAges);
    m_clockTimer->start();

    refreshGps(false);
    updateClockAndAges();
    updateNtpServerTip();
}

bool GpsLocationDialog::currentPosition(double& latitude, double& longitude) const
{
    if (m_radioModel == nullptr) {
        return false;
    }
    return aprs::parseGpsCoordinate(m_radioModel->gpsLat(), latitude)
        && aprs::parseGpsCoordinate(m_radioModel->gpsLon(), longitude);
}

void GpsLocationDialog::refreshGps(bool reportArrived)
{
    if (m_radioModel == nullptr) {
        m_fixStatusLabel->setText(tr("No radio"));
        return;
    }

    if (reportArrived || (!m_reportAge.isValid()
                          && !m_radioModel->gpsStatus().isEmpty())) {
        m_reportAge.restart();
    }

    const QString status = displayValue(m_radioModel->gpsStatus());
    const bool locked = statusIsLocked(m_radioModel->gpsStatus());
    const bool gpsAvailable = m_radioModel->hasGpsHardware()
        || !m_radioModel->gpsStatus().isEmpty();
    m_fixStatusLabel->setText(gpsAvailable ? status : tr("Not present"));

    const int tracked = qMax(0, m_radioModel->gpsTracked());
    const int visible = qMax(0, m_radioModel->gpsVisible());
    m_satelliteSummaryLabel->setText(
        visible > 0 ? QStringLiteral("%1 / %2").arg(tracked).arg(visible)
                    : QStringLiteral("—"));
    m_trackedLabel->setText(QString::number(tracked));
    m_visibleLabel->setText(QString::number(visible));
    const int ratio = visible > 0 ? qBound(0, qRound(100.0 * tracked / visible), 100) : 0;
    m_trackingRatioLabel->setText(
        visible > 0 ? tr("%1%").arg(ratio) : QStringLiteral("—"));
    m_satelliteProgress->setValue(ratio);
    m_satelliteProgress->setFormat(
        visible > 0 ? tr("%1 tracked / %2 visible").arg(tracked).arg(visible)
                    : tr("No satellite-count data"));

    const QString actualReference = referenceName(m_radioModel->oscState());
    const QString referenceLock = m_radioModel->oscLocked() ? tr("Locked") : tr("Unlocked");
    m_referenceSummaryLabel->setText(
        tr("%1 · %2").arg(actualReference, referenceLock));
    m_referenceSettingLabel->setText(referenceName(m_radioModel->oscSetting()));
    m_referenceActualLabel->setText(actualReference);
    m_referenceLockLabel->setText(referenceLock);
    m_frequencyErrorLabel->setText(displayValue(m_radioModel->gpsFreqError()));

    m_gridLabel->setText(displayValue(m_radioModel->gpsGrid()).toUpper());
    m_copyGridSquareButton->setEnabled(!m_radioModel->gpsGrid().trimmed().isEmpty());
    m_altitudeLabel->setText(displayValue(m_radioModel->gpsAltitude()));
    m_speedLabel->setText(displayValue(m_radioModel->gpsSpeed()));
    const QString track = displayValue(m_radioModel->gpsTrack());
    bool trackIsNumeric = false;
    track.toDouble(&trackIsNumeric);
    m_courseLabel->setText(trackIsNumeric ? track + QChar(0x00B0) : track);
    m_nativeCoordinatesLabel->setText(
        tr("%1 / %2").arg(displayValue(m_radioModel->gpsLat()),
                           displayValue(m_radioModel->gpsLon())));

    double latitude = 0.0;
    double longitude = 0.0;
    m_hasPosition = locked && currentPosition(latitude, longitude);
    if (m_hasPosition) {
        const bool moved = !std::isfinite(m_latitude)
            || std::abs(latitude - m_latitude) > 0.00001
            || std::abs(longitude - m_longitude) > 0.00001;
        m_latitude = latitude;
        m_longitude = longitude;
        m_latitudeLabel->setText(QString::number(latitude, 'f', 6));
        m_longitudeLabel->setText(QString::number(longitude, 'f', 6));
        m_mapView->setHomePosition(latitude, longitude,
                                   m_radioModel->callsign(), true);
        if (moved && m_mapView->isVisible()) {
            m_mapView->resetToHome();
        }
        requestAddress(latitude, longitude, false);
    } else {
        m_latitudeLabel->setText(QStringLiteral("—"));
        m_longitudeLabel->setText(QStringLiteral("—"));
        m_addressLabel->setText(tr("Waiting for a valid GPS fix"));
        m_copyAddressButton->setEnabled(false);
        m_refreshAddressButton->setEnabled(false);
    }

    if (locked && !m_wasGpsLocked) {
        m_lockBeganMs = QDateTime::currentMSecsSinceEpoch();
    } else if (!locked) {
        m_lockBeganMs = 0;
    }
    m_wasGpsLocked = locked;
    updateClockAndAges();
    updateNtpServerTip();
}

void GpsLocationDialog::updateNtpServerTip()
{
    if (m_radioModel == nullptr || m_ntpServerTipLabel == nullptr) {
        return;
    }

    const QString address = m_radioModel->gpsNtpServerAddress();
    const bool canReachLocalNtp = !address.isEmpty();
    m_ntpServerTipLabel->setVisible(canReachLocalNtp);
    if (!canReachLocalNtp) {
        m_ntpServerTipLabel->clear();
        return;
    }

    m_ntpServerTipLabel->setText(
        tr("Did you know? You can use your radio as an NTP server when satellite "
           "lock is active. Set your time server to %1. Use it only on a trusted "
           "local network.")
            .arg(address));
}

void GpsLocationDialog::requestAddress(double latitude, double longitude, bool force)
{
    if (!force && m_hasAddressQueryPosition
        && std::abs(latitude - m_addressQueryLatitude)
            <= kAddressMovementThresholdDegrees
        && std::abs(longitude - m_addressQueryLongitude)
            <= kAddressMovementThresholdDegrees) {
        return;
    }

    m_hasAddressQueryPosition = true;
    m_addressQueryLatitude = latitude;
    m_addressQueryLongitude = longitude;
    m_resolvedAddress.clear();
    m_addressLabel->setText(tr("Looking up nearest mapped address…"));
    m_copyAddressButton->setEnabled(false);
    m_refreshAddressButton->setEnabled(false);
    m_addressResolver->resolve(latitude, longitude);
}

void GpsLocationDialog::updateClockAndAges()
{
    const QDateTime utc = QDateTime::currentDateTimeUtc();
    const QDateTime local = utc.toLocalTime();
    const QLocale locale = QLocale::system();
    m_utcTimeLabel->setText(
        locale.toString(utc.date(), QLocale::ShortFormat)
        + utc.toString(QStringLiteral("  HH:mm:ss 'UTC'")));
    m_localTimeLabel->setText(
        locale.toString(local.date(), QLocale::ShortFormat)
        + local.toString(QStringLiteral("  HH:mm:ss t")));
    m_timeZoneLabel->setText(
        tr("%1 (UTC%2%3:%4)")
            .arg(local.timeZoneAbbreviation())
            .arg(local.offsetFromUtc() >= 0 ? QStringLiteral("+") : QString(QChar(0x2212)))
            .arg(std::abs(local.offsetFromUtc()) / 3600, 2, 10, QLatin1Char('0'))
            .arg((std::abs(local.offsetFromUtc()) % 3600) / 60, 2, 10, QLatin1Char('0')));

    if (m_radioModel != nullptr) {
        m_radioGpsTimeLabel->setText(displayValue(m_radioModel->gpsTime()));
        QTime radioTime = QTime::fromString(m_radioModel->gpsTime(), QStringLiteral("HH:mm:ss'Z'"));
        if (!radioTime.isValid()) {
            radioTime = QTime::fromString(m_radioModel->gpsTime(), QStringLiteral("HH:mm:ssZ"));
        }
        if (radioTime.isValid() && statusIsLocked(m_radioModel->gpsStatus())) {
            // The radio reports only whole-second GPS time. Advance that last
            // sample by its telemetry age before comparing it with the computer,
            // otherwise a healthy but infrequent GPS status update looks many
            // seconds slow. Milliseconds expose the same signed estimate in the
            // units commonly used while diagnosing FT8 DT; the UI note retains
            // the whole-second source-precision caveat.
            const qint64 reportAgeMs = m_reportAge.isValid() ? m_reportAge.elapsed() : 0;
            const QTime estimatedRadioTime = radioTime.addMSecs(reportAgeMs);
            qint64 deltaMs = utc.time().msecsTo(estimatedRadioTime);
            constexpr qint64 kHalfDayMs = 12LL * 60 * 60 * 1000;
            constexpr qint64 kDayMs = 24LL * 60 * 60 * 1000;
            if (deltaMs > kHalfDayMs) {
                deltaMs -= kDayMs;
            } else if (deltaMs < -kHalfDayMs) {
                deltaMs += kDayMs;
            }

            const qint64 absoluteDeltaMs = std::abs(deltaMs);
            const QString secondsText = locale.toString(
                static_cast<double>(absoluteDeltaMs) / 1000.0, 'f', 3);
            const QString millisecondsText = locale.toString(absoluteDeltaMs);
            if (deltaMs > 0) {
                m_clockAgreementLabel->setText(
                    tr("Radio GPS is approximately %1 seconds ahead (%2 ms)")
                        .arg(secondsText, millisecondsText));
            } else if (deltaMs < 0) {
                m_clockAgreementLabel->setText(
                    tr("Radio GPS is approximately %1 seconds behind (%2 ms)")
                        .arg(secondsText, millisecondsText));
            } else {
                m_clockAgreementLabel->setText(
                    tr("Radio GPS and computer agree (0.000 seconds; 0 ms)"));
            }
        } else {
            m_clockAgreementLabel->setText(tr("Unavailable until the GPS is locked"));
        }
    }

    if (!m_reportAge.isValid()) {
        m_freshnessLabel->setText(tr("Waiting"));
    } else {
        const qint64 ageSeconds = m_reportAge.elapsed() / 1000;
        m_freshnessLabel->setText(
            ageSeconds <= 1 ? tr("Live") : tr("%1 s old").arg(ageSeconds));
    }

    if (m_lockBeganMs > 0 && m_wasGpsLocked) {
        const qint64 seconds =
            (QDateTime::currentMSecsSinceEpoch() - m_lockBeganMs) / 1000;
        m_lockDurationLabel->setText(durationText(seconds));
    } else {
        m_lockDurationLabel->setText(tr("Not locked"));
    }
}

void GpsLocationDialog::copyGridSquare()
{
    if (m_radioModel == nullptr || m_radioModel->gpsGrid().trimmed().isEmpty()) {
        return;
    }
    QApplication::clipboard()->setText(m_radioModel->gpsGrid().trimmed().toUpper());
}

void GpsLocationDialog::copyAddress()
{
    if (m_resolvedAddress.isEmpty()) {
        return;
    }
    QApplication::clipboard()->setText(m_resolvedAddress);
}

} // namespace AetherSDR
