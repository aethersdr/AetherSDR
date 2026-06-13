#include "PskReporterMapDialog.h"

#include "core/AppSettings.h"
#include "core/MaidenheadLocator.h"
#include "core/PskReporterClient.h"
#include "map/MapView.h"
#include "models/RadioModel.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

constexpr const char* kIntervalKey = "PskReporterUpdateIntervalMs";

// Marker color per amateur band, loosely matching common band-map palettes.
QColor bandColor(qint64 freqHz)
{
    const double mhz = freqHz / 1e6;
    if (mhz < 2.0)   return QColor(0x8b, 0x45, 0x13);   // 160m brown
    if (mhz < 4.5)   return QColor(0xe0, 0x3c, 0x3c);   // 80m red
    if (mhz < 6.0)   return QColor(0xc7, 0x5e, 0xff);   // 60m violet
    if (mhz < 8.0)   return QColor(0xff, 0x8c, 0x00);   // 40m orange
    if (mhz < 11.0)  return QColor(0xb8, 0xb8, 0x00);   // 30m olive
    if (mhz < 16.0)  return QColor(0x2e, 0xb8, 0x2e);   // 20m green
    if (mhz < 19.5)  return QColor(0x00, 0xb8, 0xb8);   // 17m teal
    if (mhz < 22.5)  return QColor(0x2e, 0x7c, 0xff);   // 15m blue
    if (mhz < 26.0)  return QColor(0x9b, 0x59, 0xb6);   // 12m purple
    if (mhz < 40.0)  return QColor(0xe9, 0x1e, 0x8c);   // 10m magenta
    if (mhz < 60.0)  return QColor(0x60, 0x60, 0x60);   // 6m gray
    return QColor(0x20, 0x20, 0x20);                    // VHF+
}

QString bandName(qint64 freqHz)
{
    const double mhz = freqHz / 1e6;
    if (mhz < 2.0)   return QStringLiteral("160m");
    if (mhz < 4.5)   return QStringLiteral("80m");
    if (mhz < 6.0)   return QStringLiteral("60m");
    if (mhz < 8.0)   return QStringLiteral("40m");
    if (mhz < 11.0)  return QStringLiteral("30m");
    if (mhz < 16.0)  return QStringLiteral("20m");
    if (mhz < 19.5)  return QStringLiteral("17m");
    if (mhz < 22.5)  return QStringLiteral("15m");
    if (mhz < 26.0)  return QStringLiteral("12m");
    if (mhz < 40.0)  return QStringLiteral("10m");
    if (mhz < 60.0)  return QStringLiteral("6m");
    return QStringLiteral("VHF+");
}

} // namespace

PskReporterMapDialog::PskReporterMapDialog(RadioModel* radioModel,
                                           QWidget* parent)
    : PersistentDialog(tr("PSK Reporter Map"),
                       QStringLiteral("PskReporterMapGeometry"), parent)
    , m_radioModel(radioModel)
    , m_client(new PskReporterClient(this))
{
    setMinimumSize(720, 480);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    auto* topBar = new QHBoxLayout();
    topBar->addWidget(new QLabel(tr("Update every:"), bodyWidget()));

    m_intervalCombo = new QComboBox(bodyWidget());
    // PSK Reporter policy floors HTTP polling at 5 minutes; "Live" uses
    // their sanctioned MQTT feed instead of fast polling.
    m_intervalCombo->addItem(tr("Live (MQTT)"), PskReporterClient::kLiveMqtt);
    m_intervalCombo->addItem(tr("5 minutes"), 5 * 60 * 1000);
    m_intervalCombo->addItem(tr("10 minutes"), 10 * 60 * 1000);
    m_intervalCombo->addItem(tr("15 minutes"), 15 * 60 * 1000);
    m_intervalCombo->addItem(tr("30 minutes"), 30 * 60 * 1000);
    m_intervalCombo->addItem(tr("1 hour"), 60 * 60 * 1000);
    topBar->addWidget(m_intervalCombo);

    topBar->addStretch(1);
    m_statusLabel = new QLabel(bodyWidget());
    m_statusLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));
    topBar->addWidget(m_statusLabel);
    root->addLayout(topBar);

    m_mapView = new MapView(bodyWidget());
    root->addWidget(m_mapView, 1);

    const int savedInterval =
        AppSettings::instance()
            .value(kIntervalKey, QString::number(5 * 60 * 1000))
            .toInt();
    const int idx = m_intervalCombo->findData(savedInterval);
    m_intervalCombo->setCurrentIndex(idx >= 0 ? idx : 1);

    connect(m_intervalCombo, &QComboBox::currentIndexChanged,
            this, &PskReporterMapDialog::onIntervalChanged);
    connect(m_client, &PskReporterClient::spotsUpdated,
            this, &PskReporterMapDialog::rebuildMarkers);
    connect(m_client, &PskReporterClient::statusChanged,
            m_statusLabel, &QLabel::setText);

    if (m_radioModel != nullptr) {
        connect(m_radioModel, &RadioModel::gpsStatusChanged,
                this, [this] { updateHomeFromRadio(); });
    }
}

void PskReporterMapDialog::updateHomeFromRadio()
{
    if (m_radioModel == nullptr) {
        return;
    }
    const QString label = m_radioModel->callsign();
    bool ok = false;
    double lat = m_radioModel->gpsLat().toDouble(&ok);
    double lon = 0.0;
    if (ok) {
        lon = m_radioModel->gpsLon().toDouble(&ok);
    }
    // GPS fix preferred; fall back to the radio's grid locator.
    if (!ok || (lat == 0.0 && lon == 0.0)) {
        if (!MaidenheadLocator::toLatLon(m_radioModel->gpsGrid(), lat, lon)) {
            return;
        }
    }
    m_mapView->setHomePosition(lat, lon, label);
}

void PskReporterMapDialog::onIntervalChanged(int index)
{
    const int intervalMs = m_intervalCombo->itemData(index).toInt();
    AppSettings::instance().setValue(kIntervalKey,
                                     QString::number(intervalMs));
    restartClient();
}

void PskReporterMapDialog::restartClient()
{
    m_client->setCallsign(m_radioModel != nullptr ? m_radioModel->callsign()
                                                  : QString());
    m_client->start(m_intervalCombo->currentData().toInt());
}

void PskReporterMapDialog::rebuildMarkers()
{
    QVector<MapView::Marker> markers;
    markers.reserve(m_client->spots().size());
    for (const PskReporterSpot& spot : m_client->spots()) {
        double lat = 0.0;
        double lon = 0.0;
        if (!MaidenheadLocator::toLatLon(spot.receiverLocator, lat, lon)) {
            continue;
        }
        MapView::Marker m;
        m.lat = lat;
        m.lon = lon;
        m.label = spot.receiverCallsign;
        m.color = bandColor(spot.frequencyHz);
        m.tooltip =
            tr("%1 (%2)\n%3 %4 MHz\n%5%6")
                .arg(spot.receiverCallsign, spot.receiverLocator,
                     bandName(spot.frequencyHz),
                     QString::number(spot.frequencyHz / 1e6, 'f', 3),
                     spot.mode,
                     spot.snr > -999
                         ? tr("  SNR %1 dB").arg(spot.snr)
                         : QString());
        markers.append(m);
    }
    m_mapView->setMarkers(markers);
}

void PskReporterMapDialog::showEvent(QShowEvent* event)
{
    PersistentDialog::showEvent(event);
    updateHomeFromRadio();
    if (!m_started) {
        m_started = true;
        restartClient();
    }
}

void PskReporterMapDialog::closeEvent(QCloseEvent* event)
{
    // Stop hitting the network while the window is closed.
    m_client->stop();
    m_started = false;
    PersistentDialog::closeEvent(event);
}

} // namespace AetherSDR
