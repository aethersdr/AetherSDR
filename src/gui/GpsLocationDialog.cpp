#include "GpsLocationDialog.h"

#include "CanonIndicators.h"
#include "CanonScrollBarStyle.h"
#include "CanonTableStyle.h"
#include "AetherGpsTools.h"
#include "GpsSkyPlot.h"
#include "FramelessResizer.h"

#include "core/LocationAddressResolver.h"
#include "core/ThemeManager.h"
#include "core/aprs/AprsPacket.h"
#include "map/MapView.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QAccessible>
#include <QClipboard>
#include <QCheckBox>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace AetherSDR {

namespace {

constexpr double kAddressMovementThresholdDegrees = 0.001;

// The style guide's canon vocabulary (RFC #6226), as AetherMap and Network
// Diagnostics use it: transparent over CanonWindow's ground except the
// surfaces that carry content. The header strip and each section are raised
// cards, the metric tiles and the tip sit nested inside them, and fields sit
// on control. Section titles and focus are cyan.
constexpr const char* kAetherGpsStyle = R"(
QWidget {
    color: {{color.canon.inkSoft}};
    background: transparent;
}
QLabel {
    background: transparent;
}
QFrame[gpsHeader='true'],
QGroupBox {
    background: {{color.canon.raised}};
    border: 1px solid {{color.canon.line}};
    border-radius: 10px;
}
QFrame[gpsMetricCard='true'] {
    background: {{color.canon.nested}};
    border: 1px solid {{color.canon.line}};
    border-radius: 7px;
}
QGroupBox QGroupBox {
    background: {{color.canon.nested}};
}
QLabel[gpsRole='sectionTitle'] {
    color: {{color.canon.cyan}};
    font-size: 14px;
    font-weight: 700;
    padding-bottom: 4px;
}
QLabel[gpsRole='metricTitle'],
QLabel[gpsRole='fieldTitle'] {
    color: {{color.canon.muted}};
    font-size: 11px;
}
QLabel[gpsRole='metricValue'] {
    color: {{color.canon.ink}};
    font-size: 18px;
    font-weight: 700;
}
QLabel[gpsRole='value'] {
    color: {{color.canon.ink}};
    font-size: 13px;
    font-weight: 600;
}
QLabel[gpsRole='muted'] {
    color: {{color.canon.muted}};
    font-size: 11px;
}
QLabel[gpsRole='tip'] {
    color: {{color.canon.inkSoft}};
    background: {{color.canon.nested}};
    border: 1px solid {{color.canon.line}};
    border-radius: 6px;
    padding: 8px;
    font-size: 12px;
}
QCheckBox {
    background: transparent;
    color: {{color.canon.inkSoft}};
    spacing: 8px;
}
QPushButton {
    color: {{color.canon.cyan}};
    background: {{color.canon.control}};
    border: 1px solid {{color.canon.lineHi}};
    border-radius: 4px;
    padding: 5px 12px;
    font-weight: 600;
}
QPushButton:hover {
    background: {{color.canon.nested}};
    color: {{color.canon.aqua}};
}
QPushButton:focus {
    border-color: {{color.canon.aqua}};
}
QPushButton:disabled {
    color: {{color.canon.muted}};
    border-color: {{color.canon.line}};
    background: transparent;
}
QLineEdit {
    color: {{color.canon.ink}};
    background: {{color.canon.control}};
    border: 1px solid {{color.canon.lineHi}};
    border-radius: 4px;
    padding: 4px 8px;
    selection-background-color: {{color.canon.cyan}};
    selection-color: {{color.canon.onAccent}};
}
QLineEdit:focus {
    border-color: {{color.canon.aqua}};
}
QLineEdit:disabled {
    color: {{color.canon.muted}};
    border-color: {{color.canon.line}};
}
QProgressBar {
    color: {{color.canon.ink}};
    background: {{color.canon.control}};
    border: 1px solid {{color.canon.lineHi}};
    border-radius: 5px;
    text-align: center;
    min-height: 22px;
}
QProgressBar::chunk {
    background: {{color.background.success}};
    border-radius: 4px;
}
QScrollArea {
    background: transparent;
    border: none;
}
)";

bool validNtpInput(const QString& value)
{
    static const QRegularExpression allowed(
        QStringLiteral("^[A-Za-z0-9.-]{1,64}$"));
    return allowed.match(value.trimmed()).hasMatch();
}

QString displayValue(const QString& value)
{
    return value.trimmed().isEmpty() ? QStringLiteral("—") : value.trimmed();
}

QString fixStateText(GpsFixState state)
{
    switch (state) {
    case GpsFixState::ThreeD: return GpsLocationDialog::tr("3D");
    case GpsFixState::TwoD: return GpsLocationDialog::tr("2D");
    case GpsFixState::TimeOnly: return GpsLocationDialog::tr("Time only");
    case GpsFixState::NoFix: return GpsLocationDialog::tr("No fix");
    case GpsFixState::Unknown: return GpsLocationDialog::tr("Unknown");
    }
    return GpsLocationDialog::tr("Unknown");
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
    : CanonWindow(QStringLiteral("AetherGPS"), parent, Kind::Workspace)
    , m_radioModel(radioModel)
    , m_addressResolver(new LocationAddressResolver(this))
{
    setObjectName(QStringLiteral("gpsLocationDialog"));
    theme::setContainer(this, QStringLiteral("canon/aetherGps"));
    // Size and position persist under the key it used as a PersistentDialog,
    // so saved geometry carries over; it resizes from every edge.
    setGeometryKey(QStringLiteral("GpsLocationDialogGeometry"));
    setMinimumSize(860, 640);
    resize(1080, 760);
    FramelessResizer::install(this);

    applyCanonSheet(bodyWidget(), [] {
        return QString::fromLatin1(kAetherGpsStyle) + canonScrollBarRules() + canonIndicatorRules()
             + canonTableRules();
    });

    auto* outer = new QVBoxLayout(bodyWidget());
    outer->setContentsMargins(14, 12, 14, 14);
    outer->setSpacing(8);
    outer->addWidget(makeCanonHeader(QStringLiteral("AetherGPS")));

    auto* scroll = new QScrollArea(bodyWidget());
    scroll->setObjectName(QStringLiteral("gpsLocationScrollArea"));
    scroll->setWidgetResizable(true);
    outer->addWidget(scroll);

    // ── AetherGPS on the radio: install or remove ─────────────────────────
    // Like "Install radio health tools" in the Waveforms window, this
    // checkbox is the waveform's only control; the Waveforms list hides it.
    m_gpsToolsBar = new QFrame(bodyWidget());
    m_gpsToolsBar->setObjectName(QStringLiteral("gpsToolsBar"));
    m_gpsToolsBar->setProperty("gpsHeader", true);
    auto* toolsLayout = new QHBoxLayout(m_gpsToolsBar);
    toolsLayout->setContentsMargins(18, 10, 18, 10);
    toolsLayout->setSpacing(16);
    m_gpsToolsCheck = new QCheckBox(tr("Install AetherGPS on the radio"), m_gpsToolsBar);
    m_gpsToolsCheck->setObjectName(QStringLiteral("gpsToolsCheck"));
    m_gpsToolsCheck->setAccessibleName(tr("Install AetherGPS on the radio"));
    m_gpsToolsLabel = new QLabel(m_gpsToolsBar);
    m_gpsToolsLabel->setObjectName(QStringLiteral("gpsToolsStatus"));
    m_gpsToolsLabel->setProperty("gpsRole", QStringLiteral("muted"));
    m_gpsToolsLabel->setTextFormat(Qt::PlainText);
    m_gpsToolsLabel->setWordWrap(true);
    m_gpsToolsLabel->setAccessibleName(tr("AetherGPS on the radio"));
    toolsLayout->addWidget(m_gpsToolsCheck);
    toolsLayout->addWidget(m_gpsToolsLabel, 1);
    outer->addWidget(m_gpsToolsBar);
    connect(m_gpsToolsCheck, &QCheckBox::toggled, this, &GpsLocationDialog::onGpsToolsToggled);
    m_gpsTools = new AetherGpsTools(radioModel, this);
    connect(m_gpsTools, &AetherGpsTools::changed, this, &GpsLocationDialog::updateGpsTools);
    connect(m_gpsTools, &AetherGpsTools::installProgress, this, [this](int percent) {
        m_installPercent = percent;
        updateGpsTools();
    });
    connect(m_gpsTools, &AetherGpsTools::failed, this, [this](const QString& message) {
        m_gpsToolsLabel->setText(message);
    });

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
    m_satelliteMetricCard = makeMetricCard(
        tr("SATELLITES"), QStringLiteral("gpsSatelliteSummary"),
        tr("GPS satellites tracked and visible"), m_satelliteSummaryLabel, header);
    headerLayout->addWidget(m_satelliteMetricCard);
    m_referenceMetricCard = makeMetricCard(
        tr("10 MHz REFERENCE"), QStringLiteral("gpsReferenceSummary"),
        tr("Radio frequency reference status"), m_referenceSummaryLabel, header);
    headerLayout->addWidget(m_referenceMetricCard);
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

    m_sourceLabel = new QLabel;
    configureReadout(m_sourceLabel, QStringLiteral("gpsPositionSource"),
                     tr("Position source selected by the radio"));
    addReadoutRow(locationLayout, 1, tr("Source"), m_sourceLabel);
    m_gridLabel = new QLabel;
    configureReadout(m_gridLabel, QStringLiteral("gpsGrid"), tr("Maidenhead grid square"));
    addReadoutRow(locationLayout, 2, tr("Grid square"), m_gridLabel);
    m_latitudeLabel = new QLabel;
    configureReadout(m_latitudeLabel, QStringLiteral("gpsLatitude"), tr("GPS latitude"));
    addReadoutRow(locationLayout, 3, tr("Latitude"), m_latitudeLabel);
    m_longitudeLabel = new QLabel;
    configureReadout(m_longitudeLabel, QStringLiteral("gpsLongitude"), tr("GPS longitude"));
    addReadoutRow(locationLayout, 4, tr("Longitude"), m_longitudeLabel);
    m_nativeCoordinatesLabel = new QLabel;
    configureReadout(m_nativeCoordinatesLabel, QStringLiteral("gpsNativeCoordinates"),
                     tr("Coordinates as reported by the radio"));
    m_nativeCoordinatesLabel->setWordWrap(true);
    addReadoutRow(locationLayout, 5, tr("Radio format"), m_nativeCoordinatesLabel);
    m_altitudeLabel = new QLabel;
    configureReadout(m_altitudeLabel, QStringLiteral("gpsAltitude"), tr("GPS altitude"));
    addReadoutRow(locationLayout, 6, tr("Altitude"), m_altitudeLabel);
    m_speedLabel = new QLabel;
    configureReadout(m_speedLabel, QStringLiteral("gpsSpeed"), tr("GPS speed"));
    addReadoutRow(locationLayout, 7, tr("Speed"), m_speedLabel);
    m_courseLabel = new QLabel;
    configureReadout(m_courseLabel, QStringLiteral("gpsCourse"), tr("GPS course over ground"));
    addReadoutRow(locationLayout, 8, tr("Course"), m_courseLabel);

    auto* addressTitle = new QLabel(tr("Nearest mapped address"));
    addressTitle->setProperty("gpsRole", QStringLiteral("fieldTitle"));
    locationLayout->addWidget(addressTitle, 9, 0, Qt::AlignTop);
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
    locationLayout->addWidget(addressColumn, 9, 1, Qt::AlignTop);

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
    locationLayout->addLayout(actions, 10, 0, 1, 2, Qt::AlignTop);
    locationLayout->setRowStretch(11, 1);
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

    m_satelliteGroup = new QGroupBox(content);
    m_satelliteGroup->setAccessibleName(
        tr("Satellite Reception and Frequency Reference"));
    auto* satelliteLayout = new QGridLayout(m_satelliteGroup);
    satelliteLayout->setContentsMargins(14, 12, 14, 12);
    satelliteLayout->setHorizontalSpacing(18);
    satelliteLayout->setVerticalSpacing(7);
    satelliteLayout->setColumnStretch(1, 1);
    satelliteLayout->addWidget(makeSectionTitle(
        tr("Satellite Reception and Frequency Reference"), m_satelliteGroup),
        0, 0, 1, 2);

    m_satelliteProgress = new QProgressBar(m_satelliteGroup);
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
    m_satelliteApiNote = new QLabel(
        tr("The radio reports aggregate tracked/visible counts, but not individual "
           "satellite IDs, azimuth, elevation, or signal strength. Orbital positions "
           "are therefore intentionally not estimated."), m_satelliteGroup);
    m_satelliteApiNote->setObjectName(QStringLiteral("gpsSatelliteApiNote"));
    m_satelliteApiNote->setProperty("gpsRole", QStringLiteral("muted"));
    m_satelliteApiNote->setWordWrap(true);
    m_satelliteApiNote->setAccessibleName(tr("Satellite API limitation"));
    satelliteLayout->addWidget(m_satelliteApiNote, 10, 0, 1, 2);
    satelliteLayout->setRowStretch(11, 1);
    root->addWidget(m_satelliteGroup, 2, 0);

    // ── Sky view: each satellite, from the radio's gpsd via AetherGPS ──
    m_skyGroup = new QGroupBox(content);
    m_skyGroup->setObjectName(QStringLiteral("gpsSkyGroup"));
    m_skyGroup->setAccessibleName(tr("Sky View"));
    auto* skyLayout = new QGridLayout(m_skyGroup);
    skyLayout->setContentsMargins(14, 12, 14, 12);
    skyLayout->setHorizontalSpacing(18);
    skyLayout->setVerticalSpacing(7);
    skyLayout->addWidget(makeSectionTitle(tr("Sky View"), m_skyGroup), 0, 0, 1, 2);
    m_skyStatusLabel = new QLabel(m_skyGroup);
    m_skyStatusLabel->setObjectName(QStringLiteral("gpsSkyStatus"));
    m_skyStatusLabel->setProperty("gpsRole", QStringLiteral("muted"));
    m_skyStatusLabel->setWordWrap(true);
    m_skyStatusLabel->setTextFormat(Qt::PlainText);
    m_skyStatusLabel->setAccessibleName(tr("Sky view status"));
    skyLayout->addWidget(m_skyStatusLabel, 1, 0, 1, 2);
    m_skyPlot = new GpsSkyPlot(m_skyGroup);
    m_skyPlot->setMinimumSize(260, 260);
    skyLayout->addWidget(m_skyPlot, 2, 0);
    m_skyTable = new QTableWidget(0, 6, m_skyGroup);
    m_skyTable->setObjectName(QStringLiteral("gpsSkyTable"));
    m_skyTable->setAccessibleName(tr("Satellites"));
    m_skyTable->setHorizontalHeaderLabels({tr("System"), tr("PRN"), tr("Elevation"),
                                           tr("Azimuth"), tr("Signal"), tr("In fix")});
    m_skyTable->verticalHeader()->hide();
    m_skyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_skyTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_skyTable->setAlternatingRowColors(true);
    m_skyTable->setSortingEnabled(true);
    m_skyTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_skyTable->setMinimumHeight(260);
    skyLayout->addWidget(m_skyTable, 2, 1);
    skyLayout->setColumnStretch(0, 5);
    skyLayout->setColumnStretch(1, 6);

    // The receiver detail gpsd reports beside the satellites.
    auto* detailGrid = new QGridLayout;
    detailGrid->setHorizontalSpacing(18);
    detailGrid->setVerticalSpacing(7);
    detailGrid->setColumnStretch(1, 1);
    detailGrid->setColumnStretch(3, 1);
    int detailIndex = 0;
    auto addDetail = [&](QLabel*& label, const char* objectName, const QString& title,
                         const QString& accessibleName) {
        label = new QLabel(QStringLiteral("—"), m_skyGroup);
        configureReadout(label, QString::fromLatin1(objectName), accessibleName);
        auto* caption = new QLabel(title, m_skyGroup);
        caption->setProperty("gpsRole", QStringLiteral("fieldTitle"));
        const int row = detailIndex / 2, column = (detailIndex % 2) * 2;
        detailGrid->addWidget(caption, row, column, Qt::AlignTop);
        detailGrid->addWidget(label, row, column + 1, Qt::AlignTop);
        ++detailIndex;
    };
    addDetail(m_fixModeLabel, "gpsdFixMode", tr("Fix mode"), tr("GPS fix mode reported by gpsd"));
    addDetail(m_dopLabel, "gpsdDop", tr("HDOP / VDOP / PDOP"),
              tr("Horizontal, vertical and position dilution of precision"));
    addDetail(m_errorHorizontalLabel, "gpsdErrorHorizontal", tr("Est. horizontal error"),
              tr("Estimated horizontal position error, 95 percent"));
    addDetail(m_dopTimeLabel, "gpsdDopTime", tr("TDOP / GDOP"),
              tr("Time and geometric dilution of precision"));
    addDetail(m_errorVerticalLabel, "gpsdErrorVertical", tr("Est. vertical error"),
              tr("Estimated vertical position error, 95 percent"));
    addDetail(m_errorTimeLabel, "gpsdErrorTime", tr("Est. time error"),
              tr("Estimated time error, 95 percent"));
    addDetail(m_ppsOffsetLabel, "gpsdPpsOffset", tr("Radio clock vs PPS"),
              tr("The radio clock's offset from GPS time at the pulse per second"));
    addDetail(m_ppsSourceLabel, "gpsdPpsSource", tr("PPS source"), tr("Pulse-per-second source"));
    addDetail(m_receiverLabel, "gpsdReceiver", tr("Receiver"), tr("GPS receiver device, driver and speed"));
    addDetail(m_gpsdVersionLabel, "gpsdVersion", tr("gpsd"), tr("gpsd version on the radio"));
    skyLayout->addLayout(detailGrid, 3, 0, 1, 2);
    root->addWidget(m_skyGroup, 3, 0, 1, 2);

    m_skyClient = new GpsdSkyClient(this);
    connect(m_skyClient, &GpsdSkyClient::skyUpdated, this, &GpsLocationDialog::showSky);
    connect(m_skyClient, &GpsdSkyClient::stateChanged, this, &GpsLocationDialog::showSkyState);
    connect(m_skyClient, &GpsdSkyClient::skyStale, this, [this] {
        clearSky(tr("The radio's GPS hasn't reported satellites for 30 seconds."));
    });
    connect(m_skyClient, &GpsdSkyClient::detailUpdated, this, &GpsLocationDialog::showDetail);

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
    m_timeSourceNote = new QLabel(
        tr("The radio GPS feed supplies UTC time-of-day but no date. UTC/local dates "
           "above therefore come from the computer clock. Use Clock agreement as a "
           "coarse guide when interpreting FT8 time differential (DT), and keep the "
           "computer synchronized with NTP for precise operation."), timeGroup);
    m_timeSourceNote->setProperty("gpsRole", QStringLiteral("muted"));
    m_timeSourceNote->setWordWrap(true);
    m_timeSourceNote->setAccessibleName(tr("GPS date source explanation"));
    timeLayout->addWidget(m_timeSourceNote, 6, 0, 1, 2);
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

    m_gpsTimeControlGroup = new QGroupBox(timeGroup);
    m_gpsTimeControlGroup->setAccessibleName(tr("Radio clock synchronization"));
    auto* gpsTimeControlLayout = new QGridLayout(m_gpsTimeControlGroup);
    gpsTimeControlLayout->setContentsMargins(12, 10, 12, 10);
    gpsTimeControlLayout->setHorizontalSpacing(9);
    gpsTimeControlLayout->setVerticalSpacing(8);
    gpsTimeControlLayout->setColumnStretch(1, 1);
    gpsTimeControlLayout->addWidget(
        makeSectionTitle(tr("Radio Clock Synchronization"), m_gpsTimeControlGroup),
        0, 0, 1, 3);

    m_ntpEnabledCheck = new QCheckBox(tr("Enable NTP client"), m_gpsTimeControlGroup);
    m_ntpEnabledCheck->setObjectName(QStringLiteral("gpsNtpEnabled"));
    m_ntpEnabledCheck->setAccessibleDescription(
        tr("Enable or disable the NTP client stored by the radio"));
    gpsTimeControlLayout->addWidget(m_ntpEnabledCheck, 1, 0, 1, 3);

    auto* ntpServerTitle = new QLabel(tr("NTP server"), m_gpsTimeControlGroup);
    ntpServerTitle->setProperty("gpsRole", QStringLiteral("fieldTitle"));
    gpsTimeControlLayout->addWidget(ntpServerTitle, 2, 0);
    m_ntpServerEdit = new QLineEdit(m_gpsTimeControlGroup);
    m_ntpServerEdit->setObjectName(QStringLiteral("gpsNtpServer"));
    m_ntpServerEdit->setMaxLength(64);
    m_ntpServerEdit->setPlaceholderText(tr("pool.ntp.org"));
    m_ntpServerEdit->setAccessibleName(tr("NTP server address"));
    m_ntpServerEdit->setAccessibleDescription(
        tr("Server stored in the radio; letters, digits, dots, and hyphens only"));
    gpsTimeControlLayout->addWidget(m_ntpServerEdit, 2, 1);
    m_applyNtpServerButton = makeActionButton(
        tr("Apply"), QStringLiteral("gpsApplyNtpServer"),
        tr("Send this NTP server to the radio and read it back"), m_gpsTimeControlGroup);
    gpsTimeControlLayout->addWidget(m_applyNtpServerButton, 2, 2);

    m_gpsTimeCorrectionCheck = new QCheckBox(
        tr("Correct radio clock from GPS automatically"), m_gpsTimeControlGroup);
    m_gpsTimeCorrectionCheck->setObjectName(QStringLiteral("gpsTimeCorrection"));
    m_gpsTimeCorrectionCheck->setAccessibleDescription(
        tr("Enable or disable the GPS Time Correct setting stored by the radio"));
    gpsTimeControlLayout->addWidget(m_gpsTimeCorrectionCheck, 3, 0, 1, 3);

    m_ntpSyncButton = makeActionButton(
        tr("Sync now"), QStringLiteral("gpsNtpSyncNow"),
        tr("Ask the radio to access its configured NTP server now"), m_gpsTimeControlGroup);
    gpsTimeControlLayout->addWidget(m_ntpSyncButton, 4, 0);
    m_ntpSyncStatusLabel = new QLabel(m_gpsTimeControlGroup);
    configureReadout(m_ntpSyncStatusLabel, QStringLiteral("gpsNtpSyncStatus"),
                     tr("NTP access result reported by the radio"));
    gpsTimeControlLayout->addWidget(m_ntpSyncStatusLabel, 4, 1, 1, 2);

    auto* radioAuthorityNote = new QLabel(
        tr("These settings are stored by the radio. AetherSDR sends a change only "
           "when you use a control here, then displays the radio's read-back."),
        m_gpsTimeControlGroup);
    radioAuthorityNote->setProperty("gpsRole", QStringLiteral("muted"));
    radioAuthorityNote->setWordWrap(true);
    radioAuthorityNote->setAccessibleName(tr("Radio setting authority explanation"));
    gpsTimeControlLayout->addWidget(radioAuthorityNote, 5, 0, 1, 3);
    timeLayout->addWidget(m_gpsTimeControlGroup, 8, 0, 1, 2);
    timeLayout->setRowStretch(9, 1);
    root->addWidget(timeGroup, 2, 1);

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
        connect(m_radioModel, &RadioModel::gpsTimeSettingsChanged,
                this, &GpsLocationDialog::updateGpsTimeControls);
        // AetherGpsTools also follows connection, capabilities and the
        // radio's waveform list, and reports through changed().
        connect(m_radioModel, &RadioModel::capabilitiesChanged,
                this, [this] { refreshGps(false); });
        connect(m_ntpEnabledCheck, &QCheckBox::clicked,
                m_radioModel, &RadioModel::setGpsNtpEnabled);
        connect(m_gpsTimeCorrectionCheck, &QCheckBox::clicked,
                m_radioModel, &RadioModel::setGpsTimeCorrectionEnabled);
        connect(m_applyNtpServerButton, &QPushButton::clicked, this, [this] {
            m_radioModel->setGpsNtpServer(m_ntpServerEdit->text().trimmed());
            m_ntpServerEdit->setModified(false);   // the read-back now owns it
        });
        connect(m_ntpServerEdit, &QLineEdit::textChanged, this,
                [this](const QString& value) {
                    m_applyNtpServerButton->setEnabled(validNtpInput(value));
                });
        connect(m_ntpServerEdit, &QLineEdit::returnPressed,
                m_applyNtpServerButton, &QPushButton::click);
        connect(m_ntpSyncButton, &QPushButton::clicked,
                m_radioModel, &RadioModel::requestGpsNtpSync);
    }

    m_clockTimer = new QTimer(this);
    m_clockTimer->setInterval(1000);
    connect(m_clockTimer, &QTimer::timeout,
            this, &GpsLocationDialog::updateClockAndAges);
    m_clockTimer->start();

    refreshGps(false);
    updateClockAndAges();
    updateNtpServerTip();
    updateGpsTimeControls();
}

void GpsLocationDialog::showEvent(QShowEvent* event)
{
    CanonWindow::showEvent(event);
    updateGpsTools();
}

void GpsLocationDialog::hideEvent(QHideEvent* event)
{
    CanonWindow::hideEvent(event);
    updateSkySource();
}

void GpsLocationDialog::updateGpsTools()
{
    const bool connected = m_radioModel && m_radioModel->isConnected();
    m_gpsToolsInstalled = m_gpsTools->installed();
    // A radio family without waveforms (an IC-705) has nothing to install.
    m_gpsToolsBar->setVisible(!connected || m_gpsTools->radioTakesWaveforms());
    const bool busy = m_gpsTools->busy();
    const QString reason = m_gpsTools->blockerReason();
    {
        const QSignalBlocker block(m_gpsToolsCheck);
        m_gpsToolsCheck->setChecked(m_gpsToolsInstalled);
    }
    m_gpsToolsCheck->setEnabled(reason.isEmpty() && !busy);

    const QString about = tr("Installs a small tool on the radio that shows each satellite, the "
                             "receiver's accuracy and PPS timing here, and shares the radio's GPS, "
                             "including its position, with gpsd and NMEA programs on your network.");
    if (!busy) {
        m_installPercent = -1;
    }
    if (busy) {
        if (m_gpsToolsInstalled) {
            m_gpsToolsLabel->setText(tr("Removing AetherGPS from the radio…"));
        } else if (m_installPercent >= 0 && m_installPercent < 100) {
            m_gpsToolsLabel->setText(tr("Installing AetherGPS on the radio… %1%").arg(m_installPercent));
        } else {
            m_gpsToolsLabel->setText(tr("Installing AetherGPS on the radio…"));
        }
    } else if (!reason.isEmpty()) {
        m_gpsToolsLabel->setText(reason);
    } else if (!m_gpsToolsInstalled) {
        m_gpsToolsLabel->setText(about);
    } else {
        m_gpsToolsLabel->setText(tr("Installed. Clear the box to remove it from the radio."));
    }
    // The reason a disabled checkbox gives, on the accessible channel too.
    m_gpsToolsCheck->setAccessibleDescription(!reason.isEmpty() ? reason : about);
    m_gpsToolsCheck->setToolTip(m_gpsToolsCheck->accessibleDescription());

    updateSkySource();
}

void GpsLocationDialog::onGpsToolsToggled(bool install)
{
    if (install) {
        m_gpsTools->install();
    } else {
        m_gpsTools->remove();
    }
}

void GpsLocationDialog::showDetail(const GpsdDetail& d)
{
    const QLocale locale;
    auto value = [&](double v, int decimals, const QString& unit = {}) {
        if (!std::isfinite(v)) {
            return QStringLiteral("—");
        }
        const QString n = locale.toString(v, 'f', decimals);
        return unit.isEmpty() ? n : tr("%1 %2").arg(n, unit);
    };
    static const char* const kModes[] = {QT_TR_NOOP("Unknown"), QT_TR_NOOP("No fix"), QT_TR_NOOP("2D"),
                                         QT_TR_NOOP("3D")};
    m_fixModeLabel->setText(tr(kModes[qBound(0, d.mode, 3)]));
    m_dopLabel->setText(tr("%1 / %2 / %3").arg(value(d.hdop, 1), value(d.vdop, 1), value(d.pdop, 1)));
    m_dopTimeLabel->setText(tr("%1 / %2").arg(value(d.tdop, 1), value(d.gdop, 1)));
    m_errorHorizontalLabel->setText(value(d.ephM, 0, tr("m")));
    m_errorVerticalLabel->setText(value(d.epvM, 0, tr("m")));
    m_errorTimeLabel->setText(std::isfinite(d.eptS) ? value(d.eptS * 1000.0, 1, tr("ms"))
                                                    : QStringLiteral("—"));
    if (d.hasPps) {
        const double us = double(d.ppsOffsetNs) / 1000.0;
        m_ppsOffsetLabel->setText(std::abs(us) >= 1000.0
                                      ? tr("%1 ms").arg(locale.toString(us / 1000.0, 'f', 3))
                                      : tr("%1 µs").arg(locale.toString(us, 'f', 1)));
    } else {
        m_ppsOffsetLabel->setText(QStringLiteral("—"));
    }
    m_ppsSourceLabel->setText(d.ppsDevice.isEmpty() ? QStringLiteral("—") : d.ppsDevice);
    QStringList receiver;
    if (!d.device.isEmpty()) {
        receiver << d.device;
    }
    if (!d.driver.isEmpty()) {
        receiver << d.driver;
    }
    if (d.bps > 0) {
        receiver << tr("%1 baud").arg(d.bps);
    }
    m_receiverLabel->setText(receiver.isEmpty() ? QStringLiteral("—") : receiver.join(QStringLiteral(" · ")));
    const QString release = m_skyClient->gpsdRelease();
    m_gpsdVersionLabel->setText(release.isEmpty() ? QStringLiteral("—") : release);
}

void GpsLocationDialog::updateSkySource()
{
    const bool takesWaveforms = m_radioModel && m_radioModel->isConnected()
        && m_radioModel->backendCapabilities().hasWaveforms;
    // The Sky View is what the waveform adds: it shows only while installed.
    m_skyGroup->setVisible(takesWaveforms && m_gpsToolsInstalled);
    const bool reachable = takesWaveforms && m_gpsToolsInstalled
        && gpsdsky::reachableRadio(m_radioModel->isWan(), m_radioModel->radioAddress());
    if (!isVisible() || !reachable) {
        m_skyClient->stop();
        clearSky(tr("Each satellite is shown when the radio is on this computer's local network "
                    "or reached through Remote Access."));
        return;
    }
    m_skyClient->start(m_radioModel->radioAddress());
}

void GpsLocationDialog::showSkyState(GpsdSkyClient::State state, const QString& detail)
{
    switch (state) {
    case GpsdSkyClient::State::Connecting:
        if (!m_skyHasData) {
            m_skyStatusLabel->setText(tr("Connecting to the radio's GPS…"));
        }
        break;
    case GpsdSkyClient::State::Streaming:
        if (!m_skyHasData) {
            m_skyStatusLabel->setText(tr("Waiting for the radio's GPS to report satellites…"));
        }
        break;
    case GpsdSkyClient::State::Unavailable: {
        // The detail may be the relay's own words, which end without a stop.
        QString text = detail.trimmed();
        if (!text.isEmpty() && !text.endsWith(QLatin1Char('.'))) {
            text += QLatin1Char('.');
        }
        // The radio lists AetherGPS (this view shows only then), so when
        // nothing answered it is most likely still starting.
        if (!m_skyClient->relayReached()) {
            text += tr(" AetherGPS may still be starting on the radio; trying again every 10 seconds.");
        }
        clearSky(text.trimmed());
        break;
    }
    case GpsdSkyClient::State::Stopped:
        break;
    }
}

void GpsLocationDialog::clearSky(const QString& status)
{
    showSky({});
    showDetail({});
    m_skyStatusLabel->setText(status);
}

void GpsLocationDialog::showSky(const QList<GpsSatellite>& satellites)
{
    m_skyHasData = !satellites.isEmpty();
    m_skyPlot->setSatellites(satellites);
    // The limitation note describes the radio's own status; the sky view
    // fills the gap while it reports.
    m_satelliteApiNote->setVisible(!m_skyHasData);

    const QLocale locale;
    auto& tm = ThemeManager::instance();
    m_skyTable->setSortingEnabled(false);
    m_skyTable->setRowCount(int(satellites.size()));
    int heard = 0, used = 0;
    for (int row = 0; row < satellites.size(); ++row) {
        const GpsSatellite& s = satellites.at(row);
        heard += s.heard();
        used += s.used;
        const gpsdsky::Constellation c = gpsdsky::constellation(s);
        auto number = [](double v) {
            auto* item = new QTableWidgetItem;
            if (std::isfinite(v)) {
                item->setData(Qt::DisplayRole, qRound(v));
            } else {
                item->setText(QStringLiteral("—"));
            }
            item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            return item;
        };
        auto* system = new QTableWidgetItem(gpsdsky::constellationName(c));
        system->setForeground(tm.color(m_skyTable, GpsSkyPlot::colorToken(c)));
        m_skyTable->setItem(row, 0, system);
        auto* prn = new QTableWidgetItem;
        prn->setData(Qt::DisplayRole, s.prn);
        prn->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_skyTable->setItem(row, 1, prn);
        m_skyTable->setItem(row, 2, number(s.elevationDeg));
        m_skyTable->setItem(row, 3, number(s.azimuthDeg));
        m_skyTable->setItem(row, 4, number(s.heard() ? s.snrDbHz : NAN));
        m_skyTable->setItem(row, 5, new QTableWidgetItem(s.used ? tr("Yes") : QString()));
    }
    m_skyTable->setSortingEnabled(true);
    // Every report sets the line, so an empty one doesn't leave old counts.
    QString text = m_skyHasData
        ? tr("%1 listed · %2 heard · %3 used in the fix").arg(satellites.size()).arg(heard).arg(used)
        : tr("The radio's GPS lists no satellites.");
    const QString release = m_skyClient->gpsdRelease();
    if (!release.isEmpty()) {
        text += tr(" · gpsd %1").arg(release);
    }
    m_skyStatusLabel->setText(text);
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
    const RadioCapabilities caps = m_radioModel->backendCapabilities();
    const bool hasSatelliteTelemetry = caps.hasGpsSatelliteTelemetry;
    const bool hasFrequencyReference = caps.hasGpsFrequencyReference;
    const bool gpsAvailable = m_radioModel->hasGpsHardware()
        || !m_radioModel->gpsStatus().isEmpty()
        || m_radioModel->gpsFixState() != GpsFixState::Unknown;
    const QString fixText = gpsAvailable
        ? fixStateText(m_radioModel->gpsFixState()) : tr("Not present");
    QString fixDescription = tr("Receiver status: %1. Fix state: %2.")
        .arg(status, fixText);
    if (m_radioModel->gpsFixState() == GpsFixState::Unknown) {
        fixDescription += tr(" The radio has not reported a recognized fix state.");
    }
    if (m_radioModel->gpsFixState() == GpsFixState::TimeOnly) {
        fixDescription += tr(" The radio reports time-only mode without a position fix. "
                             "Time validity and frequency-reference lock are shown separately.");
    }
    const bool fixChanged = m_fixStatusLabel->text() != fixText
        || m_fixStatusLabel->accessibleDescription() != fixDescription;
    m_fixStatusLabel->setText(fixText);
    m_fixStatusLabel->setToolTip(fixDescription);
    m_fixStatusLabel->setAccessibleDescription(fixDescription);
    if (fixChanged) {
        QAccessibleEvent event(m_fixStatusLabel, QAccessible::DescriptionChanged);
        QAccessible::updateAccessibility(&event);
    }
    m_sourceLabel->setText(displayValue(m_radioModel->gpsSource()));
    m_satelliteMetricCard->setVisible(hasSatelliteTelemetry);
    m_referenceMetricCard->setVisible(hasFrequencyReference);
    m_satelliteGroup->setVisible(hasSatelliteTelemetry || hasFrequencyReference);

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
    m_hasPosition = m_radioModel->gpsPositionValid()
        && currentPosition(latitude, longitude);
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
        // Reacquisition at the same coordinates must restore the address too.
        // The resolver can reuse its cache, but the displayed query is retired.
        m_hasAddressQueryPosition = false;
        m_resolvedAddress.clear();
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
    if (!m_radioModel->gpsDate().isEmpty()) {
        m_timeSourceNote->setText(
            tr("The radio reports a complete UTC date and time. Clock agreement is "
               "a coarse whole-second comparison; keep the computer synchronized "
               "for digital modes that depend on precise timing."));
    } else {
        m_timeSourceNote->setText(
            tr("This radio feed supplies UTC time-of-day but no date. UTC/local dates "
               "above therefore come from the computer clock. Clock agreement is a "
               "coarse guide; keep the computer synchronized for precise operation."));
    }
    updateClockAndAges();
    updateNtpServerTip();
    updateGpsTimeControls();
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

void GpsLocationDialog::updateGpsTimeControls()
{
    if (m_radioModel == nullptr || m_gpsTimeControlGroup == nullptr) {
        return;
    }
    const bool supported = m_radioModel->isConnected()
        && m_radioModel->backendCapabilities().hasGpsTimeConfiguration;
    m_gpsTimeControlGroup->setVisible(supported);
    if (!supported) {
        return;
    }

    const QSignalBlocker ntpBlock(m_ntpEnabledCheck);
    const QSignalBlocker correctionBlock(m_gpsTimeCorrectionCheck);
    m_ntpEnabledCheck->setChecked(m_radioModel->gpsNtpEnabled());
    m_gpsTimeCorrectionCheck->setChecked(
        m_radioModel->gpsTimeCorrectionEnabled());
    // isModified() is the operator's unsent edit; a periodic read-back must
    // not replace it just because focus moved to the Apply button.
    if (!m_ntpServerEdit->isModified()) {
        const QSignalBlocker serverBlock(m_ntpServerEdit);
        m_ntpServerEdit->setText(m_radioModel->gpsNtpServer());
    }
    m_applyNtpServerButton->setEnabled(validNtpInput(m_ntpServerEdit->text()));
    m_ntpSyncStatusLabel->setText(
        displayValue(m_radioModel->gpsNtpSyncStatus()));
    const bool canSync = m_radioModel->gpsNtpEnabled()
        && !m_radioModel->gpsNtpServer().isEmpty();
    m_ntpSyncButton->setEnabled(canSync);
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
        const QString radioDateTime = m_radioModel->gpsDate().isEmpty()
            ? m_radioModel->gpsTime()
            : tr("%1  %2").arg(m_radioModel->gpsDate(), m_radioModel->gpsTime());
        m_radioGpsTimeLabel->setText(displayValue(radioDateTime));
        QTime radioTime = QTime::fromString(m_radioModel->gpsTime(), QStringLiteral("HH:mm:ss'Z'"));
        if (!radioTime.isValid()) {
            radioTime = QTime::fromString(m_radioModel->gpsTime(), QStringLiteral("HH:mm:ssZ"));
        }
        if (radioTime.isValid() && m_radioModel->gpsTimeValid()) {
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
            m_clockAgreementLabel->setText(tr("Unavailable until the radio reports GPS time"));
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
