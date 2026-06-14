#include "PskReporterMapDialog.h"

#include "core/AppSettings.h"
#include "core/MaidenheadLocator.h"
#include "core/PskReporterClient.h"
#include "map/MapView.h"
#include "models/RadioModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QToolTip>
#include <QtMath>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

// PSK Reporter map settings live in one nested-JSON AppSettings blob under a
// single root key (Constitution Principle V) rather than separate flat keys.
constexpr const char* kSettingsKey = "PskReporter";

QJsonObject pskSettings()
{
    const QString json =
        AppSettings::instance().value(kSettingsKey, QString{}).toString();
    if (json.isEmpty())
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

void writePskSetting(const QString& field, const QJsonValue& value)
{
    QJsonObject obj = pskSettings();
    obj.insert(field, value);
    AppSettings::instance().setValue(
        kSettingsKey,
        QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
}

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

// Initial great-circle bearing from point 1 to point 2, degrees 0-360.
double bearingDeg(double lat1, double lon1, double lat2, double lon2)
{
    const double p1 = qDegreesToRadians(lat1);
    const double p2 = qDegreesToRadians(lat2);
    const double dl = qDegreesToRadians(lon2 - lon1);
    const double y = std::sin(dl) * std::cos(p2);
    const double x = std::cos(p1) * std::sin(p2)
                   - std::sin(p1) * std::cos(p2) * std::cos(dl);
    return std::fmod(qRadiansToDegrees(std::atan2(y, x)) + 360.0, 360.0);
}

// SNR color: decoded spots are never "bad", so this is a strong/medium/weak
// ramp (green → orange → gray), all readable on a light tooltip background.
QString snrColor(int snr)
{
    if (snr >= -5)  return QStringLiteral("#1b5e20");  // strong
    if (snr >= -15) return QStringLiteral("#bf6000");  // medium
    return QStringLiteral("#616161");                  // weak but decoded
}

// Compact, human-readable age of a report.
QString relativeAge(qint64 reportEpoch)
{
    const qint64 age = QDateTime::currentSecsSinceEpoch() - reportEpoch;
    if (age < 60)    return PskReporterMapDialog::tr("just now");
    if (age < 3600)  return PskReporterMapDialog::tr("%1m ago").arg(age / 60);
    if (age < 86400) return PskReporterMapDialog::tr("%1h ago").arg(age / 3600);
    return PskReporterMapDialog::tr("%1d ago").arg(age / 86400);
}

// Minimal-footprint hover/click card. Short stacked lines keep the box
// narrow so neighbouring spots stay visible; SNR is the headline figure.
//   <b>W1ABC</b>  FN42hn
//   20m · 14.074 MHz · FT8
//   −12 dB · 5,432 km @ 048°
//   14:23:01Z · 2m ago
QString buildSpotCard(const PskReporterSpot& spot, bool hasHome,
                      double homeLat, double homeLon, double spotLat,
                      double spotLon)
{
    const QString freq =
        QString::number(spot.frequencyHz / 1e6, 'f', 3);
    QString html = QStringLiteral("<div style='white-space:nowrap;'>");

    // Line 1 — identity.
    html += QStringLiteral("<b>%1</b>&nbsp;&nbsp;"
                           "<span style='color:gray;'>%2</span>")
                .arg(spot.receiverCallsign.toHtmlEscaped(),
                     spot.receiverLocator.toHtmlEscaped());

    // Line 2 — RF.
    html += QStringLiteral("<br>%1 · %2 MHz · %3")
                .arg(bandName(spot.frequencyHz), freq,
                     spot.mode.toHtmlEscaped());

    // Line 3 — signal + geometry.
    QString line3;
    if (spot.snr > -999) {
        line3 = QStringLiteral("<b style='color:%1;'>%2 dB</b>")
                    .arg(snrColor(spot.snr))
                    .arg(spot.snr);
    }
    if (hasHome) {
        const double km =
            MaidenheadLocator::distanceKm(homeLat, homeLon, spotLat, spotLon);
        const double brg = bearingDeg(homeLat, homeLon, spotLat, spotLon);
        const QString geo =
            PskReporterMapDialog::tr("%L1 km @ %2°")
                .arg(qRound(km))
                .arg(qRound(brg), 3, 10, QLatin1Char('0'));
        line3 += line3.isEmpty() ? geo : (QStringLiteral(" · ") + geo);
    }
    if (!line3.isEmpty()) {
        html += QStringLiteral("<br>") + line3;
    }

    // Line 4 — time (absolute UTC is always correct; age is glanceable).
    html += QStringLiteral("<br><span style='color:gray;'>%1 · %2</span>")
                .arg(QDateTime::fromSecsSinceEpoch(spot.flowStartSeconds)
                         .toUTC()
                         .toString(QStringLiteral("hh:mm:ss'Z'")),
                     relativeAge(spot.flowStartSeconds));

    html += QStringLiteral("</div>");
    return html;
}

} // namespace

PskReporterMapDialog::PskReporterMapDialog(RadioModel* radioModel,
                                           QWidget* parent)
    : PersistentDialog(tr("PSK Reporter"),
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

    m_pathsCheck = new QCheckBox(tr("Paths"), bodyWidget());
    m_pathsCheck->setToolTip(tr("Draw great-circle paths from your station to each receiver"));
    m_pathsCheck->setChecked(pskSettings().value("showPaths").toBool(true));
    topBar->addWidget(m_pathsCheck);

    topBar->addStretch(1);
    m_dxLabel = new QLabel(bodyWidget());
    topBar->addWidget(m_dxLabel);
    topBar->addSpacing(10);
    m_statusLabel = new QLabel(bodyWidget());
    m_statusLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));
    topBar->addWidget(m_statusLabel);
    root->addLayout(topBar);

    m_mapView = new MapView(bodyWidget());
    m_mapView->setPathsVisible(m_pathsCheck->isChecked());
    {
        QVector<QPair<QString, QColor>> legend;
        for (const char* m : { "FT8", "FT4", "WSPR", "JS8", "CW", "PSK",
                               "RTTY", "SSB", "Other" }) {
            legend.append({ QString::fromLatin1(m),
                            modeColor(QString::fromLatin1(m)) });
        }
        m_mapView->setLegend(legend);
    }
    root->addWidget(m_mapView, 1);

    connect(m_pathsCheck, &QCheckBox::toggled, this, [this](bool on) {
        writePskSetting("showPaths", on);
        m_mapView->setPathsVisible(on);
    });
    connect(m_mapView, &MapView::markerClicked, this,
            [](const MapView::Marker& marker) {
                if (!marker.clickInfo.isEmpty()) {
                    QToolTip::showText(QCursor::pos(), marker.clickInfo);
                }
            });

    // Empty-state guidance: if nothing has been heard a couple of minutes
    // after starting, explain what to expect instead of a blank map.
    m_emptyStateTimer = new QTimer(this);
    m_emptyStateTimer->setSingleShot(true);
    m_emptyStateTimer->setInterval(2 * 60 * 1000);
    connect(m_emptyStateTimer, &QTimer::timeout, this, [this] {
        if (m_client->spots().isEmpty()) {
            m_statusLabel->setText(
                tr("No reports yet — transmit (e.g. FT8) and reports "
                   "typically appear within 1–2 minutes."));
        }
    });

    const int savedInterval =
        pskSettings().value("updateIntervalMs").toInt(PskReporterClient::kLiveMqtt);
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
    writePskSetting("updateIntervalMs", intervalMs);
    restartClient();
}

void PskReporterMapDialog::restartClient()
{
    m_client->setCallsign(m_radioModel != nullptr ? m_radioModel->callsign()
                                                  : QString());
    m_client->start(m_intervalCombo->currentData().toInt());
    m_emptyStateTimer->start();
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
        // Same compact card for hover and click.
        const QString card = buildSpotCard(
            spot, m_mapView->hasHomePosition(),
            m_mapView->homeLat(), m_mapView->homeLon(), lat, lon);
        m.tooltip = card;
        m.clickInfo = card;
        markers.append(m);
    }
    m_mapView->setMarkers(markers);

    // Status enrichment: spot count and farthest receiver.
    QString dx;
    if (m_mapView->hasHomePosition()) {
        double bestKm = -1.0;
        QString bestCall;
        for (const MapView::Marker& m : markers) {
            const double km = MaidenheadLocator::distanceKm(
                m_mapView->homeLat(), m_mapView->homeLon(), m.lat, m.lon);
            if (km > bestKm) {
                bestKm = km;
                bestCall = m.label;
            }
        }
        if (bestKm >= 0.0) {
            dx = tr("%n spot(s)", nullptr, markers.size())
                 + tr(" • farthest: %1 %L2 km")
                       .arg(bestCall)
                       .arg(qRound(bestKm));
        }
    }
    if (dx.isEmpty() && !markers.isEmpty()) {
        dx = tr("%n spot(s)", nullptr, markers.size());
    }
    m_dxLabel->setText(dx);
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
