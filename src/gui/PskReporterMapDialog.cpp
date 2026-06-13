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

// Normalize a PSK Reporter / ADIF mode string to one of the selector's
// mode groups.
QString modeGroup(const QString& mode)
{
    const QString m = mode.toUpper();
    if (m.startsWith(QLatin1String("FT8")))  return QStringLiteral("FT8");
    if (m.startsWith(QLatin1String("FT4")))  return QStringLiteral("FT4");
    if (m.startsWith(QLatin1String("WSPR")) || m == QLatin1String("FST4W"))
        return QStringLiteral("WSPR");
    if (m.startsWith(QLatin1String("JS8")))  return QStringLiteral("JS8");
    if (m == QLatin1String("CW"))            return QStringLiteral("CW");
    if (m.startsWith(QLatin1String("PSK")) || m.startsWith(QLatin1String("BPSK"))
        || m.startsWith(QLatin1String("QPSK")))
        return QStringLiteral("PSK");
    if (m.startsWith(QLatin1String("RTTY"))) return QStringLiteral("RTTY");
    if (m == QLatin1String("SSB") || m == QLatin1String("USB")
        || m == QLatin1String("LSB"))
        return QStringLiteral("SSB");
    return QStringLiteral("Other");
}

// Marker color per mode group — saturated hues chosen to stand out on the
// pastel OSM basemap (markers also carry a dark outline + white label halo).
QColor modeColor(const QString& mode)
{
    const QString g = modeGroup(mode);
    if (g == QLatin1String("FT8"))  return QColor(0xe5, 0x39, 0x35);  // red
    if (g == QLatin1String("FT4"))  return QColor(0xfb, 0x8c, 0x00);  // orange
    if (g == QLatin1String("WSPR")) return QColor(0x8e, 0x24, 0xaa);  // purple
    if (g == QLatin1String("JS8"))  return QColor(0x00, 0x89, 0x7b);  // teal
    if (g == QLatin1String("CW"))   return QColor(0x1e, 0x88, 0xe5);  // blue
    if (g == QLatin1String("PSK"))  return QColor(0xd8, 0x1b, 0x60);  // pink
    if (g == QLatin1String("RTTY")) return QColor(0x6d, 0x4c, 0x41);  // brown
    if (g == QLatin1String("SSB"))  return QColor(0x43, 0xa0, 0x47);  // green
    return QColor(0x37, 0x47, 0x4f);                                  // slate
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

    topBar->addWidget(new QLabel(tr("Band:"), bodyWidget()));
    m_bandCombo = new QComboBox(bodyWidget());
    m_bandCombo->addItem(tr("All"));
    for (const char* b : { "160m", "80m", "60m", "40m", "30m", "20m",
                           "17m", "15m", "12m", "10m", "6m", "VHF+" }) {
        m_bandCombo->addItem(QString::fromLatin1(b));
    }
    topBar->addWidget(m_bandCombo);

    topBar->addWidget(new QLabel(tr("Mode:"), bodyWidget()));
    m_modeCombo = new QComboBox(bodyWidget());
    m_modeCombo->addItem(tr("All"));
    for (const char* m : { "FT8", "FT4", "WSPR", "JS8", "CW", "PSK",
                           "RTTY", "SSB", "Other" }) {
        m_modeCombo->addItem(QString::fromLatin1(m));
    }
    topBar->addWidget(m_modeCombo);

    topBar->addSpacing(12);
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
            .value(kIntervalKey,
                   QString::number(PskReporterClient::kLiveMqtt))
            .toInt();
    const int idx = m_intervalCombo->findData(savedInterval);
    m_intervalCombo->setCurrentIndex(idx >= 0 ? idx : 0);

    connect(m_intervalCombo, &QComboBox::currentIndexChanged,
            this, &PskReporterMapDialog::onIntervalChanged);
    connect(m_bandCombo, &QComboBox::currentIndexChanged,
            this, [this] { rebuildMarkers(); });
    connect(m_modeCombo, &QComboBox::currentIndexChanged,
            this, [this] { rebuildMarkers(); });
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
    const QString bandFilter = m_bandCombo->currentIndex() > 0
                                   ? m_bandCombo->currentText()
                                   : QString();
    const QString modeFilter = m_modeCombo->currentIndex() > 0
                                   ? m_modeCombo->currentText()
                                   : QString();
    for (const PskReporterSpot& spot : m_client->spots()) {
        if (!bandFilter.isEmpty() && bandName(spot.frequencyHz) != bandFilter) {
            continue;
        }
        if (!modeFilter.isEmpty() && modeGroup(spot.mode) != modeFilter) {
            continue;
        }
        double lat = 0.0;
        double lon = 0.0;
        if (!MaidenheadLocator::toLatLon(spot.receiverLocator, lat, lon)) {
            continue;
        }
        MapView::Marker m;
        m.lat = lat;
        m.lon = lon;
        m.label = spot.receiverCallsign;
        m.color = modeColor(spot.mode);
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
