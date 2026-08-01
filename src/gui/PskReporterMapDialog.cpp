#include "PskReporterMapDialog.h"

#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/aprs/AprsPacket.h"
#include "core/LogManager.h"
#include "core/MaidenheadLocator.h"
#include "core/PropForecastClient.h"
#include "core/PskReporterClient.h"
#include "core/TxKeyingMarker.h"
#include "core/WsprBeacon.h"
#include "map/MapView.h"
#include "models/EqualizerModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QToolTip>
#include <QtMath>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <limits>

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

// HF band-condition pill color, matching PropDashboardDialog's palette.
QString bandConditionColor(const QString& condition)
{
    if (condition == QLatin1String("Good")) return QStringLiteral("#66d19e");
    if (condition == QLatin1String("Fair")) return QStringLiteral("#f2c14e");
    if (condition == QLatin1String("Poor")) return QStringLiteral("#ff8c6b");
    return QStringLiteral("#7f93a5");
}

// Short labels for the four N0NBH band groups (matching PropForecastDetail
// index order: 0=80m-40m, 1=30m-20m, 2=17m-15m, 3=12m-10m).
const char* const kBandGroupLabels[4] = { "80-40m", "30-20m", "17-15m", "12-10m" };

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

// Muted text color and the SNR highlight, tuned for the dark hover card.
constexpr const char* kCardMuted = "#b8b8b8";
constexpr const char* kSnrColor  = "#ff8c00";  // dark orange — easy to spot

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
                           "<span style='color:%2;'>%3</span>")
                .arg(spot.receiverCallsign.toHtmlEscaped(),
                     QString::fromLatin1(kCardMuted),
                     spot.receiverLocator.toHtmlEscaped());

    // Line 2 — RF.
    html += QStringLiteral("<br>%1 · %2 MHz · %3")
                .arg(bandName(spot.frequencyHz), freq,
                     spot.mode.toHtmlEscaped());

    // Line 3 — signal + geometry. SNR is always dark orange for visibility.
    QString line3;
    if (spot.snr > -999) {
        line3 = QStringLiteral("<b style='color:%1;'>%2 dB</b>")
                    .arg(QString::fromLatin1(kSnrColor))
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
    html += QStringLiteral("<br><span style='color:%1;'>%2 · %3</span>")
                .arg(QString::fromLatin1(kCardMuted),
                     QDateTime::fromSecsSinceEpoch(spot.flowStartSeconds)
                         .toUTC()
                         .toString(QStringLiteral("hh:mm:ss'Z'")),
                     relativeAge(spot.flowStartSeconds));

    html += QStringLiteral("</div>");
    return html;
}

} // namespace

PskReporterMapDialog::PskReporterMapDialog(AudioEngine* audioEngine,
                                           RadioModel* radioModel,
                                           PropForecastClient* propForecast,
                                           QWidget* parent)
    : PersistentDialog(tr("PSK Reporter"),
                       QStringLiteral("PskReporterMapGeometry"), parent)
    , m_audioEngine(audioEngine)
    , m_radioModel(radioModel)
    , m_client(new PskReporterClient(this))
    , m_propForecast(propForecast)
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

    topBar->addWidget(new QLabel(tr("Lookback:"), bodyWidget()));
    m_lookbackCombo = new QComboBox(bodyWidget());
    m_lookbackCombo->addItem(tr("15 min"), 15 * 60);
    m_lookbackCombo->addItem(tr("30 min"), 30 * 60);
    m_lookbackCombo->addItem(tr("1 hour"), 60 * 60);
    m_lookbackCombo->addItem(tr("2 hours"), 2 * 60 * 60);
    m_lookbackCombo->addItem(tr("4 hours"), 4 * 60 * 60);
    m_lookbackCombo->addItem(tr("8 hours"), 8 * 60 * 60);
    topBar->addWidget(m_lookbackCombo);

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

    // Persistent reception stats, pinned to the top-right corner.
    topBar->addStretch(1);
    m_dxLabel = new QLabel(bodyWidget());
    m_dxLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    topBar->addWidget(m_dxLabel);
    root->addLayout(topBar);

    auto* beaconBox = new QGroupBox(tr("WSPR beacon"), bodyWidget());
    beaconBox->setAccessibleName(tr("WSPR beacon transmitter"));
    auto* beaconRow = new QHBoxLayout(beaconBox);
    beaconRow->setContentsMargins(6, 4, 6, 4);
    beaconRow->setSpacing(6);

    beaconRow->addWidget(new QLabel(tr("Call:"), beaconBox));
    m_beaconCallsign = new QLineEdit(beaconBox);
    m_beaconCallsign->setMaxLength(6);
    m_beaconCallsign->setFixedWidth(76);
    m_beaconCallsign->setAccessibleName(tr("WSPR callsign"));
    m_beaconCallsign->setAccessibleDescription(
        tr("Standard callsign with a one-digit prefix"));
    beaconRow->addWidget(m_beaconCallsign);

    beaconRow->addWidget(new QLabel(tr("Grid:"), beaconBox));
    m_beaconGrid = new QLineEdit(beaconBox);
    m_beaconGrid->setMaxLength(4);
    m_beaconGrid->setFixedWidth(58);
    m_beaconGrid->setAccessibleName(tr("WSPR grid locator"));
    m_beaconGrid->setAccessibleDescription(
        tr("Four-character Maidenhead locator, for example CN85"));
    // The grid had no persistence at all. updateBeaconDefaults() prefilled it
    // from RadioModel::gpsGrid(), which is a FlexRadio GPSDO reading — a radio
    // without one leaves it empty, so the operator retyped their locator every
    // time the window opened and a beacon armed with a blank grid was rejected
    // by the encoder. Power and tone were already saved here; this was the one
    // beacon field that was not.
    m_beaconGrid->setText(
        pskSettings().value("beaconGrid").toString().trimmed().toUpper());
    beaconRow->addWidget(m_beaconGrid);

    beaconRow->addWidget(new QLabel(tr("Band:"), beaconBox));
    m_beaconBand = new QComboBox(beaconBox);
    struct WsprBand {
        const char* name;
        double dialMhz;
    };
    static constexpr WsprBand kWsprBands[] = {
        {"160m", 1.836600}, {"80m", 3.568600}, {"40m", 7.038600},
        {"30m", 10.138700}, {"20m", 14.095600}, {"17m", 18.104600},
        {"15m", 21.094600}, {"12m", 24.924600}, {"10m", 28.124600},
        {"6m", 50.293000}
    };
    for (const WsprBand& band : kWsprBands) {
        m_beaconBand->addItem(QString::fromLatin1(band.name), band.dialMhz);
    }
    m_beaconBand->setCurrentText(QStringLiteral("20m"));
    m_beaconBand->setAccessibleName(tr("WSPR band"));
    m_beaconBand->setAccessibleDescription(
        tr("Selects the standard WSPR USB dial frequency"));
    beaconRow->addWidget(m_beaconBand);

    beaconRow->addWidget(new QLabel(tr("Reported:"), beaconBox));
    m_beaconPower = new QComboBox(beaconBox);
    for (const int dbm : {0, 3, 7, 10, 13, 17, 20, 23, 27, 30,
                          33, 37, 40, 43, 47, 50, 53, 57, 60}) {
        m_beaconPower->addItem(tr("%1 dBm").arg(dbm), dbm);
    }
    const int savedPowerIndex = m_beaconPower->findData(
        pskSettings().value("beaconPowerDbm").toInt(30));
    m_beaconPower->setCurrentIndex(savedPowerIndex >= 0 ? savedPowerIndex : 9);
    m_beaconPower->setAccessibleName(tr("WSPR reported power"));
    m_beaconPower->setAccessibleDescription(
        tr("Transmitter power encoded in the WSPR message; this does not change RF power"));
    m_beaconPower->setToolTip(
        tr("Power encoded in the message; this does not change the radio's RF power"));
    beaconRow->addWidget(m_beaconPower);

    beaconRow->addWidget(new QLabel(tr("Offset:"), beaconBox));
    m_beaconTone = new QDoubleSpinBox(beaconBox);
    m_beaconTone->setRange(1400.0, 1600.0);
    m_beaconTone->setDecimals(1);
    m_beaconTone->setSingleStep(1.0);
    m_beaconTone->setSuffix(tr(" Hz"));
    m_beaconTone->setValue(
        pskSettings().value("beaconToneHz").toDouble(1500.0));
    m_beaconTone->setAccessibleName(tr("WSPR audio tone frequency"));
    m_beaconTone->setAccessibleDescription(
        tr("Audio tone from 1400 to 1600 hertz"));
    // This is the frequency of the LOWEST of the four tones, which is what
    // WSJT-X's Tx-frequency box means too. Naming it "center" (as this did)
    // was not just loose wording — the generator centred the constellation on
    // it, putting every spot 2.2 Hz below where the same number in WSJT-X
    // would have put it.
    //
    // The range stays 1400–1600 because that is WSJT-X's own WSPR range under
    // the same convention: at the top of it symbol 3 lands at 1604.4 Hz, just
    // outside the 200 Hz sub-band, exactly as it does there. Narrowing to
    // 1595.6 would be a defensible change but it would be OURS rather than the
    // oracle's, and the centred convention this replaces overran the top too
    // (1600 → 1602.2) while ALSO overrunning the bottom (1400 → 1397.8), which
    // this fixes.
    m_beaconTone->setToolTip(
        tr("Audio offset above the selected USB dial frequency, at the lowest "
           "of the four tones — the same convention as WSJT-X"));
    beaconRow->addWidget(m_beaconTone);

    beaconRow->addWidget(new QLabel(tr("Level:"), beaconBox));
    m_beaconLevel = new QSpinBox(beaconBox);
    // Hard-coded at -20 dBFS before this. WSJT-X generates at full scale and
    // attenuates digitally through its Pwr slider (SoundOutput::setAttenuation);
    // the equivalent knob here had no UI at all, so an operator who came out
    // underdriven had nothing to reach for. Ceiling of -3 rather than 0 keeps a
    // little headroom ahead of the radio's own TX chain.
    m_beaconLevel->setRange(-60, -3);
    m_beaconLevel->setSingleStep(1);
    m_beaconLevel->setSuffix(tr(" dBFS"));
    m_beaconLevel->setValue(
        pskSettings().value("beaconLevelDbFs").toInt(-20));
    m_beaconLevel->setAccessibleName(tr("WSPR transmit audio level"));
    m_beaconLevel->setAccessibleDescription(
        tr("Generated audio level in decibels full scale, from -60 to -3"));
    m_beaconLevel->setToolTip(
        tr("Level of the generated audio into the transmit chain; raise it if "
           "the radio comes out underdriven"));
    beaconRow->addWidget(m_beaconLevel);

    m_beaconButton = new QPushButton(tr("Transmit once"), beaconBox);
    m_beaconButton->setAutoDefault(false);
    markTxKeying(m_beaconButton);
    m_beaconButton->setAccessibleName(tr("Transmit one WSPR beacon"));
    m_beaconButton->setAccessibleDescription(
        tr("Arms one transmission for the next even UTC minute"));
    beaconRow->addWidget(m_beaconButton);

    m_beaconStatus = new QLabel(tr("Idle"), beaconBox);
    m_beaconStatus->setMinimumWidth(155);
    m_beaconStatus->setAccessibleName(tr("WSPR beacon status"));
    beaconRow->addWidget(m_beaconStatus, 1);
    root->addWidget(beaconBox);

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

    // Bottom row: forecasted HF band conditions justified to the bottom-left
    // (reusing the propagation-forecast N0NBH/hamqsl day/night ratings), with
    // the transient update-status text pushed to the bottom-right corner.
    auto* bottomBar = new QHBoxLayout();
    bottomBar->setSpacing(6);
    // Band-condition pills snap to the bottom-left corner — no leading title
    // label (it was near-invisible in dark themes and pushed the pills off
    // the corner); day/night context lives in each pill's tooltip.
    if (m_propForecast != nullptr) {
        for (int i = 0; i < 4; ++i) {
            auto* pill = new QLabel(QString::fromLatin1(kBandGroupLabels[i]),
                                    bodyWidget());
            pill->setAlignment(Qt::AlignCenter);
            m_bandCondPills[i] = pill;
            bottomBar->addWidget(pill);
        }
        connect(m_propForecast, &PropForecastClient::detailUpdated, this,
                [this] { updateBandConditions(); });
    }
    bottomBar->addStretch(1);
    m_statusLabel = new QLabel(bodyWidget());
    m_statusLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));
    m_statusLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bottomBar->addWidget(m_statusLabel);
    // Connection indicator pinned to the bottom-right corner: "MQTT"/"HTTP"
    // plus a status bullet (green=connected w/ data, yellow=no data,
    // red=no good connection).
    m_connLabel = new QLabel(bodyWidget());
    m_connLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bottomBar->addSpacing(10);
    bottomBar->addWidget(m_connLabel);
    root->addLayout(bottomBar);

    connect(m_client, &PskReporterClient::connectionStateChanged,
            this, &PskReporterMapDialog::updateConnectionIndicator);
    updateConnectionIndicator();

    if (m_propForecast != nullptr) {
        updateBandConditions();  // paint from cache if already fetched
    }

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

    const int savedLookback = pskSettings().value("lookbackSec").toInt(60 * 60);
    const int lbIdx = m_lookbackCombo->findData(savedLookback);
    m_lookbackCombo->setCurrentIndex(lbIdx >= 0 ? lbIdx : 2);  // default 1h
    m_client->setLookbackSeconds(m_lookbackCombo->currentData().toInt());

    // Debounce rapid Lookback changes into a single deep HTTP query, so
    // spinning through options doesn't hammer PSK Reporter (which 503s).
    m_lookbackDebounce = new QTimer(this);
    m_lookbackDebounce->setSingleShot(true);
    m_lookbackDebounce->setInterval(750);
    connect(m_lookbackDebounce, &QTimer::timeout, this, [this] {
        m_client->setLookbackSeconds(m_lookbackCombo->currentData().toInt());
    });

    connect(m_intervalCombo, &QComboBox::currentIndexChanged,
            this, &PskReporterMapDialog::onIntervalChanged);
    connect(m_lookbackCombo, &QComboBox::currentIndexChanged,
            this, &PskReporterMapDialog::onLookbackChanged);
    connect(m_bandCombo, &QComboBox::currentIndexChanged,
            this, [this] { rebuildMarkers(); });
    connect(m_modeCombo, &QComboBox::currentIndexChanged,
            this, [this] { rebuildMarkers(); });
    connect(m_client, &PskReporterClient::spotsUpdated,
            this, &PskReporterMapDialog::rebuildMarkers);
    connect(m_client, &PskReporterClient::statusChanged,
            m_statusLabel, &QLabel::setText);
    connect(m_beaconButton, &QPushButton::clicked,
            this, &PskReporterMapDialog::scheduleBeacon);
    connect(m_beaconPower, &QComboBox::currentIndexChanged, this, [this] {
        writePskSetting("beaconPowerDbm", m_beaconPower->currentData().toInt());
    });
    // Grid persists here, next to the other beacon fields.
    connect(m_beaconGrid, &QLineEdit::editingFinished, this, [this] {
        const QString grid = m_beaconGrid->text().trimmed().toUpper();
        m_beaconGrid->setText(grid);
        // Empty is "no change" (see the callsign handler); nothing to validate
        // and nothing to erase.
        if (grid.isEmpty()) {
            return;
        }
        // Validate BEFORE persisting. Writing first meant a typo was saved and
        // then silently ignored: updateHomeFromRadio() fails to decode it and
        // returns having changed nothing, so every path stayed drawn from the
        // PREVIOUS origin while the field showed the new value. The operator
        // sees a saved grid and a map that disagrees with it, with nothing
        // saying which one is wrong — and the beacon would later refuse to
        // encode the same string. (PR #4537 review.)
        double lat = 0.0, lon = 0.0;
        if (!MaidenheadLocator::toLatLon(grid, lat, lon)) {
            m_beaconStatus->setText(
                tr("“%1” is not a valid grid locator — expected 4 characters "
                   "like DM06").arg(grid));
            // Put the last good value back rather than leaving an unusable
            // string in a field the beacon will read at arm time.
            m_beaconGrid->setText(
                pskSettings().value("beaconGrid").toString().trimmed().toUpper());
            return;
        }
        writePskSetting("beaconGrid", grid);
        // This grid is also the map's home position when the radio has no GPS,
        // so moving it has to move the marker and redraw the paths — otherwise
        // the operator fixes their locator and the map keeps the old geometry
        // (or, first time, stays path-less) until the window is reopened.
        updateHomeFromRadio();
        rebuildMarkers();
    });
    // The callsign writes through to the STATION callsign rather than to a
    // beacon-private copy. There is one operator callsign, and splitting it
    // would let the beacon transmit one identity while PSK Reporter queried
    // another — which is exactly the confusing half-state this dialog was in:
    // it queried RadioModel::callsign() for the map and read this field for the
    // beacon, so on a radio that stores no callsign the map came up empty while
    // the beacon happily transmitted whatever had been typed here.
    //
    // setStationCallsign() emits callsignChanged, which restarts the PSK client
    // against the new callsign and refreshes the home marker — so typing it in
    // either place now fixes both.
    connect(m_beaconCallsign, &QLineEdit::editingFinished, this, [this] {
        const QString call = m_beaconCallsign->text().trimmed().toUpper();
        m_beaconCallsign->setText(call);
        // Empty is "no change", never "erase". Writing through on empty would
        // let clearing this field wipe the persisted StationCallsign — one
        // stray select-all-delete and PSK Reporter, the beacon and QRZ
        // own-callsign lookup all lose the station's identity, with no undo.
        // (PR #4537 review.) Clearing the field is how an operator retypes it;
        // it is not how they retire a callsign.
        if (call.isEmpty()) {
            return;
        }
        if (m_radioModel != nullptr && call != m_radioModel->callsign()) {
            if (m_radioModel->usesFlexCommandPlane()) {
                m_radioModel->sendCommand("radio callsign " + call);
            }
            m_radioModel->setStationCallsign(call);
        }
    });
    // Selecting a band tunes the receiver to that WSPR sub-band.
    //
    // This used to update the status text and nothing else, on the reasoning
    // that selecting is a choice rather than a command. In the operator's seat
    // that reads as a broken control: the label says "40m · 7.038600 MHz USB on
    // transmit" while the dial sits on whatever it was, there is no way to look
    // at the sub-band before committing to it, and the entire band change —
    // NCO, band-filter relays, TX oscillator — then happens inside the last
    // seconds before a 111.6-second transmission. Tuning here moves that jump
    // to a moment when the operator is watching, and makes "is anything
    // decoding on this band right now?" answerable before arming.
    //
    // Only the dial moves. Mode, slice passband and the station TX filter are
    // still applied by applyBeaconBand() at arm time, because those are the
    // parts that disturb a listening operator's setup, and they are restored
    // afterwards.
    //
    // Operator intent only: updateBeaconDefaults() drives this combo from the
    // slice frequency behind a QSignalBlocker, so a programmatic sync cannot
    // reach here and tune the radio back at itself.
    connect(m_beaconBand, &QComboBox::currentIndexChanged, this, [this] {
        const double dialMhz = m_beaconBand->currentData().toDouble();
        m_beaconStatus->setText(
            tr("%1 · %2 MHz USB on transmit")
                .arg(m_beaconBand->currentText())
                .arg(dialMhz, 0, 'f', 6));
        if (m_beaconArmed || m_radioModel == nullptr) {
            return;   // armed: applyBeaconBand() owns the dial until it stops
        }
        TransmitModel& tx = m_radioModel->transmitModel();
        if (tx.isTransmitting() || tx.isTuning()) {
            return;   // never move the dial out from under a live carrier
        }
        SliceModel* slice = m_radioModel->txSlice();
        if (slice == nullptr || slice->isLocked()) {
            return;   // tuneAndRecenter() would refuse anyway, and warn
        }
        slice->tuneAndRecenter(dialMhz);
    });
    connect(m_beaconLevel, &QSpinBox::valueChanged, this, [](int dbfs) {
        writePskSetting("beaconLevelDbFs", dbfs);
    });
    connect(m_beaconTone, &QDoubleSpinBox::valueChanged, this, [](double hz) {
        writePskSetting("beaconToneHz", hz);
    });

    m_beaconTimer = new QTimer(this);
    m_beaconTimer->setInterval(50);
    connect(m_beaconTimer, &QTimer::timeout,
            this, &PskReporterMapDialog::updateBeaconState);

    if (m_radioModel != nullptr) {
        connect(m_radioModel, &RadioModel::gpsStatusChanged,
                this, [this] { updateHomeFromRadio(); });
        // Pick up a late-arriving or edited callsign without a reopen.
        // restartClient() (not setCallsign alone) so a callsign that was
        // empty when the window opened actually starts the client.
        connect(m_radioModel, &RadioModel::callsignChanged, this,
                [this] {
                    if (m_started) {
                        restartClient();
                    }
                    updateHomeFromRadio();
                    updateBeaconDefaults();
                });
        connect(&m_radioModel->transmitModel(),
                &TransmitModel::transmittingChanged, this, [this](bool tx) {
            if (!tx && m_beaconTransmitting) {
                stopBeacon(tr("Stopped: transmitter unkeyed"));
            }
        });
    }
    updateBeaconDefaults();
}

void PskReporterMapDialog::updateBeaconDefaults()
{
    if (m_radioModel == nullptr || m_beaconArmed) {
        return;
    }
    if (m_beaconCallsign->text().trimmed().isEmpty()) {
        m_beaconCallsign->setText(m_radioModel->callsign().trimmed().toUpper());
    }
    if (m_beaconGrid->text().trimmed().isEmpty()) {
        // A GPSDO-derived locator is worth keeping: it seeds the field on a
        // radio that has one, and then persists so a later session on a radio
        // WITHOUT one (or after the fix disappears) still has the operator's
        // grid rather than an empty box the encoder will reject.
        const QString fromGps =
            m_radioModel->gpsGrid().trimmed().left(4).toUpper();
        if (!fromGps.isEmpty()) {
            m_beaconGrid->setText(fromGps);
            writePskSetting("beaconGrid", fromGps);
        }
    }
    const SliceModel* slice = m_radioModel->txSlice();
    if (slice != nullptr) {
        const QSignalBlocker blocker(m_beaconBand);
        int closest = 0;
        double closestDistance = std::numeric_limits<double>::max();
        for (int i = 0; i < m_beaconBand->count(); ++i) {
            const double distance =
                std::abs(m_beaconBand->itemData(i).toDouble() - slice->frequency());
            if (distance < closestDistance) {
                closest = i;
                closestDistance = distance;
            }
        }
        m_beaconBand->setCurrentIndex(closest);
    }
}

void PskReporterMapDialog::setBeaconControlsEnabled(bool enabled)
{
    m_beaconCallsign->setEnabled(enabled);
    m_beaconGrid->setEnabled(enabled);
    m_beaconBand->setEnabled(enabled);
    m_beaconPower->setEnabled(enabled);
    m_beaconTone->setEnabled(enabled);
    m_beaconLevel->setEnabled(enabled);
}

// Retune/reshape the TX slice for the selected WSPR band, at ARM time.
//
// Called only from scheduleBeacon(). This comment used to say selecting a band
// "must not retune the operator's slice" — that is no longer true and had
// become the opposite of the code: the band combo now tunes the dial on
// selection, so the operator can look at the sub-band (and its SWR) before
// committing to 111.6 s of transmission.
//
// What is still deferred to here is everything that disturbs a listening
// setup and gets restored afterwards: the slice MODE, the slice passband, and
// the station-wide TX filter. The dial is not restored — the operator asked to
// go there.
bool PskReporterMapDialog::applyBeaconBand()
{
    if (m_radioModel == nullptr) {
        return false;
    }
    TransmitModel& tx = m_radioModel->transmitModel();
    if (tx.isTransmitting() || tx.isTuning()) {
        return false;
    }
    SliceModel* slice = m_radioModel->txSlice();
    if (slice == nullptr || slice->isLocked()) {
        return false;
    }

    // `transmit filter_low/high` is a station-wide setting unrelated to this
    // slice, so remember it and hand it back in stopBeacon(). Without that the
    // operator's SSB TX stays stuck in a 600 Hz passband after one beacon.
    if (!m_beaconTxFilterSaved) {
        m_beaconPrevTxFilterLow = tx.txFilterLow();
        m_beaconPrevTxFilterHigh = tx.txFilterHigh();
        m_beaconTxFilterSaved = true;
    }

    // The speech chain is deliberately NOT borrowed here — it is taken at the
    // key, in reassertBeaconChannel(). See borrowBeaconSpeechChain().

    // Standard WSPR dial frequencies use upper sideband on every band. Keep
    // both the slice display passband and radio TX passband comfortably around
    // the selectable 1400–1600 Hz WSPR offset.
    //
    // DIGU is what actually selects the transmit sideband, and it is the one
    // line here that every family honours: a Flex takes it as slice mode, and
    // Hl2Backend maps it to the WDSP TX channel's mode. `transmit filter_*`
    // below is Flex station state — a host-modulating backend derives its TX
    // passband from the mode instead (DIGU → 150…3000 Hz), which contains the
    // WSPR offset, so the request is advisory there rather than dropped-and-
    // wrong. The generated tone is a single 4-FSK carrier ~6 Hz wide; nothing
    // about the frame depends on the narrower passband being applied.
    slice->tuneAndRecenter(m_beaconBand->currentData().toDouble());
    slice->setMode(QStringLiteral("DIGU"));
    slice->setFilterWidth(1200, 1800);
    tx.setTxFilter(1200, 1800);
    return true;
}

// Re-send mode and both passbands, and report whether this is still the
// channel the operator armed.
//
// One push at arm time is not enough, for two reasons that compound.
//
// A cross-band `slice tune` makes the radio recall freq/mode/filters from its
// BAND STACK, asynchronously, and it may recreate the slice while doing it
// (flexlib-oracle §6.2; #2824). That recall can land after the mode= and filt
// we sent immediately behind the tune, and when it does it wins — leaving the
// beacon pointed at a USB slice with whatever passband the band stack held.
// SliceModel::setMode() also deliberately declines to push a `filt` on the
// Flex path, on the reasoning that the radio heals the passband from its own
// per-mode memory, so the mode echo carries a filter of the radio's choosing.
//
// And arming happens up to a full 120 s before the key. Anything at all can
// move the slice in that window — another client, a band button, a profile
// load — and until now nothing looked again.
//
// So this runs immediately before requestPttOn(), and the generated lead-in is
// silence, so the radio has that lead-in to apply what we just sent before the
// first symbol goes out.
//
// How much lead-in that is depends on how late the tick was: a full second on
// time, and NOTHING once we are a second or more past the boundary, where
// updateBeaconState() drops the pre-roll to zero and skips into the frame
// instead. That is the weakest case and it is deliberate — a late tick means
// the GUI thread was stalled, which is when a stale channel is most likely,
// but padding the lead-in to buy settling time would put the lateness straight
// back into DT, which is the thing the slot-referenced lead-in exists to
// remove. Losing a couple of hundred ms of the first sync symbol to a mode
// that lands late costs less than a frame that reports a second of DT.
bool PskReporterMapDialog::reassertBeaconChannel(QString* reason)
{
    const auto fail = [reason](const QString& text) {
        if (reason != nullptr) *reason = text;
        return false;
    };
    if (m_radioModel == nullptr) {
        return fail(tr("No radio"));
    }
    SliceModel* slice = m_radioModel->txSlice();
    if (slice == nullptr) {
        return fail(tr("TX slice went away"));
    }
    if (slice->isLocked()) {
        return fail(tr("TX slice was locked"));
    }
    // Never write station TX state under a live carrier. applyBeaconBand()
    // refuses on exactly this at arm time and the same has to hold here,
    // because the transmitter can be taken by MOX, DAX, TCI or a tune in the
    // up-to-120 s between the two. Writing `transmit set filter_low/high`
    // mid-over reshapes the operator's voice to a 600 Hz passband in the
    // middle of a word.
    TransmitModel& tx = m_radioModel->transmitModel();
    if (tx.isTransmitting() || tx.isTuning()) {
        return fail(tr("transmitter is in use"));
    }

    // A dial that moved is the one thing NOT to paper over. Re-tuning here
    // would kick off a fresh band-stack recall with only the pre-roll to
    // settle in, and transmitting 111.6 s on a frequency the operator did not
    // choose is worse than not transmitting at all.
    const double wantMhz = m_beaconBand->currentData().toDouble();
    constexpr double kToleranceMhz = 0.000002;   // 2 Hz
    if (std::abs(slice->frequency() - wantMhz) > kToleranceMhz) {
        return fail(tr("TX slice moved to %1 MHz")
                        .arg(slice->frequency(), 0, 'f', 6));
    }

    if (slice->mode() != QStringLiteral("DIGU")) {
        qCInfo(lcGui) << "WSPR: TX slice was" << slice->mode()
                      << "at key time, not DIGU — re-asserting";
    }
    slice->setMode(QStringLiteral("DIGU"));
    slice->setFilterWidth(1200, 1800);
    tx.setTxFilter(1200, 1800);
    borrowBeaconSpeechChain(tx);
    return true;
}

// Switch off the station audio processing that would misshape the frame, and
// remember what to hand back in restoreBorrowedTxState().
//
// Everything here is station-wide and mode-independent: a Flex does not bypass
// the speech processor, the compander or the TX equalizer because a slice says
// DIGU. Left on, they operate on a constant-envelope 4-FSK tone — the compander
// pumps its level over the frame, the processor adds intermodulation products
// either side of the carrier, and the EQ tilts it — which is precisely why
// WSJT-X's own operating guidance is to switch all of it off for digital modes.
//
// Taken at the KEY rather than at arm, for the same reason mode and the
// passbands are re-asserted here: arming happens up to 120 s before the key,
// and up to ~6 minutes once the two permitted deferrals are spent. A borrow
// made at arm is stale by the time it matters — anything that turns the
// processor back on inside that window transmits through it, which is the
// "pushed once and never checked" failure this whole function exists to close.
// Reading the values here also means the saved copy is the operator's real
// state at key time, so the restore cannot hand back something two minutes old.
//
// The secondary benefit is that the operator's own station is untouched while
// merely armed: a beacon waiting for its slot no longer silently strips the
// speech processor — or VOX, which would leave a VOX operator's microphone dead
// with nothing on screen saying why — from a station they are still using.
//
// The generated lead-in gives the radio the same settling time these commands'
// neighbours get; see the note above on how much lead-in that is.
void PskReporterMapDialog::borrowBeaconSpeechChain(TransmitModel& tx)
{
    if (m_beaconTxChainSaved || m_radioModel == nullptr
        || !m_radioModel->usesFlexCommandPlane()) {
        return;
    }
    EqualizerModel& eq = m_radioModel->equalizerModel();
    m_beaconPrevSpeechProc = tx.speechProcessorEnable();
    m_beaconPrevCompander = tx.dexpOn();
    m_beaconPrevVox = tx.voxEnable();
    m_beaconPrevTxEq = eq.txEnabled();
    m_beaconTxChainSaved = true;
    if (m_beaconPrevSpeechProc) tx.setSpeechProcessorEnable(false);
    if (m_beaconPrevCompander) tx.setDexp(false);
    // VOX is not audio shaping — it is a second thing that can key and unkey
    // the transmitter. Ours is a 111.6 s frame held by an explicit MOX, and a
    // VOX release part-way through would truncate it.
    if (m_beaconPrevVox) tx.setVoxEnable(false);
    if (m_beaconPrevTxEq) eq.setTxEnabled(false);
}

// Restore the station-wide TX state the beacon borrowed: the transmit
// passband and the speech chain. The slice frequency and mode are deliberately
// left on the WSPR channel — the operator asked to go there — but none of this
// is slice state.
void PskReporterMapDialog::restoreBorrowedTxState()
{
    if (m_radioModel == nullptr) {
        m_beaconTxFilterSaved = false;
        m_beaconTxChainSaved = false;
        return;
    }
    TransmitModel& tx = m_radioModel->transmitModel();
    if (m_beaconTxFilterSaved) {
        m_beaconTxFilterSaved = false;
        tx.setTxFilter(m_beaconPrevTxFilterLow, m_beaconPrevTxFilterHigh);
    }
    if (m_beaconTxChainSaved) {
        m_beaconTxChainSaved = false;
        // Only what was actually on gets switched back on, so a restore can
        // never enable something the operator had off.
        if (m_beaconPrevSpeechProc) tx.setSpeechProcessorEnable(true);
        if (m_beaconPrevCompander) tx.setDexp(true);
        if (m_beaconPrevVox) tx.setVoxEnable(true);
        if (m_beaconPrevTxEq) m_radioModel->equalizerModel().setTxEnabled(true);
    }
}

void PskReporterMapDialog::scheduleBeacon()
{
    if (m_beaconArmed || m_beaconTransmitting) {
        stopBeacon(tr("Cancelled"));
        return;
    }
    if (m_audioEngine == nullptr || m_radioModel == nullptr) {
        m_beaconStatus->setText(tr("TX audio is unavailable"));
        return;
    }
    if (m_audioEngine->isRadeMode()) {
        m_beaconStatus->setText(tr("Stop RADE first"));
        return;
    }

    // Ask the connected radio family whether it can key at all before any of
    // the Flex-shaped preconditions below. An RX-only backend (RFC §6,
    // capabilities().canTransmit == false) has no transmitter to check.
    if (!m_radioModel->backendCapabilities().canTransmit) {
        m_beaconStatus->setText(tr("This radio cannot transmit"));
        return;
    }
    TransmitModel& tx = m_radioModel->transmitModel();
    if (tx.isTransmitting() || tx.isTuning()) {
        m_beaconStatus->setText(tr("Transmitter is already in use"));
        return;
    }
    SliceModel* slice = m_radioModel->txSlice();
    if (slice == nullptr) {
        m_beaconStatus->setText(tr("No TX slice is selected"));
        return;
    }
    if (slice->isLocked()) {
        m_beaconStatus->setText(tr("TX slice is locked"));
        return;
    }
    const int timeoutMs = tx.interlockTimeout();
    if (!WsprBeacon::isInterlockTimeoutSufficient(timeoutMs)) {
        m_beaconStatus->setText(
            tr("Radio TX timeout is %1 s; set Radio Setup → TX → Timeout to at least 120 s")
                .arg(timeoutMs / 1000));
        return;
    }

    const WsprBeacon::EncodeResult encoded = WsprBeacon::encode(
        m_beaconCallsign->text(), m_beaconGrid->text(),
        m_beaconPower->currentData().toInt());
    if (!encoded) {
        m_beaconStatus->setText(encoded.error);
        return;
    }

    // Reassert the visible band/mode/filter selection in case another client
    // changed the TX slice after the operator selected the WSPR band.
    if (!applyBeaconBand()) {
        m_beaconStatus->setText(tr("WSPR TX audio route is unavailable"));
        return;
    }
    if (!m_radioModel->prepareWsprTransmit()) {
        restoreBorrowedTxState();  // applyBeaconBand() already borrowed it
        m_beaconStatus->setText(tr("WSPR TX audio route is unavailable"));
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    m_beaconSlotMs = ((nowMs / kBeaconSlotMs) + 1) * kBeaconSlotMs;
    m_beaconDeferrals = 0;
    m_beaconDeferReason.clear();
    m_beaconArmed = true;
    m_beaconTransmitting = false;
    m_beaconButton->setText(tr("Cancel"));
    setBeaconControlsEnabled(false);
    m_beaconTimer->start();
    updateBeaconState();
}

void PskReporterMapDialog::stopBeacon(const QString& status)
{
    const bool ownedTransmit = m_beaconTransmitting;
    m_beaconTimer->stop();
    m_beaconArmed = false;
    m_beaconTransmitting = false;
    m_beaconSlotMs = 0;
    m_beaconStopDeadlineMs = 0;
    m_beaconDeferrals = 0;
    m_beaconDeferReason.clear();
    if (ownedTransmit && m_radioModel != nullptr
        && m_radioModel->transmitModel().isTransmitting()) {
        m_radioModel->transmitModel().requestPttOff(
            TransmitModel::PttSource::Wspr);
    }
    if (m_radioModel != nullptr) {
        m_radioModel->releaseWsprTransmit();
    }
    restoreBorrowedTxState();
    // Keep the generator active (and therefore holding silence) through the
    // local unkey command so an external DAX source cannot leak into the tail.
    if (m_audioEngine != nullptr && m_audioEngine->wsprBeacon() != nullptr) {
        m_audioEngine->wsprBeacon()->stop();
        QMetaObject::invokeMethod(
            m_audioEngine, &AudioEngine::stopWsprPump, Qt::QueuedConnection);
    }
    m_beaconButton->setText(tr("Transmit once"));
    setBeaconControlsEnabled(true);
    m_beaconStatus->setText(status);
}

void PskReporterMapDialog::deferBeaconToNextSlot(const QString& reason)
{
    // Stay armed: the borrowed TX filter, `transmit dax`, and DAX TX stream
    // ownership taken in scheduleBeacon() all remain held, so the next boundary
    // only has to re-check readiness. Computed from the current time rather
    // than by adding one slot to m_beaconSlotMs, so an arbitrarily long stall
    // lands on the next real boundary instead of one already in the past.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    m_beaconSlotMs = ((nowMs / kBeaconSlotMs) + 1) * kBeaconSlotMs;
    ++m_beaconDeferrals;
    // Carried by the countdown for the whole wait. Setting it as the status here
    // would be overwritten by the next 50 ms tick and never be read.
    m_beaconDeferReason = reason;
}

void PskReporterMapDialog::updateBeaconState()
{
    if (!m_beaconArmed || m_audioEngine == nullptr
        || m_radioModel == nullptr) {
        return;
    }

    WsprBeacon* beacon = m_audioEngine->wsprBeacon();
    if (m_beaconTransmitting) {
        if (!m_radioModel->hasWsprTxStream()) {
            stopBeacon(tr("Stopped: WSPR TX audio ownership was lost"));
            return;
        }
        if (QDateTime::currentMSecsSinceEpoch() > m_beaconStopDeadlineMs) {
            stopBeacon(tr("Stopped: audio timeout"));
            return;
        }
        if (beacon == nullptr || !beacon->isActive()) {
            stopBeacon(tr("Stopped: audio source unavailable"));
            return;
        }
        if (beacon->isComplete()) {
            stopBeacon(tr("Complete"));
            return;
        }
        const int symbol = std::max(0, beacon->currentSymbol());
        m_beaconStatus->setText(
            tr("Transmitting · symbol %1/%2")
                .arg(std::min(symbol + 1, WsprBeacon::kSymbolCount))
                .arg(WsprBeacon::kSymbolCount));
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 remainingMs = std::max<qint64>(0, m_beaconSlotMs - nowMs);
    if (remainingMs > 0) {
        m_beaconStatus->setText(
            tr("%1 · starts in %2.%3 s")
                .arg(m_beaconDeferReason.isEmpty() ? tr("Armed")
                                                   : m_beaconDeferReason)
                .arg(remainingMs / 1000)
                .arg((remainingMs % 1000) / 100));
        return;
    }

    // Never start a frame we cannot finish inside its own slot. The GUI tick can
    // be arbitrarily late — a suspend/resume, a modal stall, or a load spike —
    // and a 111.6 s frame started well past the boundary runs into the next
    // slot and cannot be decoded against this one. Beyond the tolerance a WSPR
    // decoder holds against the slot edge, wait for the next boundary instead.
    if (nowMs - m_beaconSlotMs > kBeaconMaxSlotLatenessMs) {
        deferBeaconToNextSlot(tr("Re-armed · slot boundary was missed"));
        return;
    }

    const WsprBeacon::EncodeResult encoded = WsprBeacon::encode(
        m_beaconCallsign->text(), m_beaconGrid->text(),
        m_beaconPower->currentData().toInt());
    if (!encoded || beacon == nullptr) {
        stopBeacon(encoded ? tr("WSPR audio unavailable") : encoded.error);
        return;
    }
    // Arming a few hundred ms before a boundary leaves the radio's `stream
    // create` still in flight at slot time. That is an ordinary race, not a
    // failure, so roll to the next slot rather than throwing the operator's
    // arming away — but give up after a bounded number of slots so a stream
    // that never arrives cannot leave the beacon armed indefinitely.
    if (!m_radioModel->hasWsprTxStream()) {
        if (m_beaconDeferrals >= kBeaconMaxDeferrals) {
            stopBeacon(tr("Stopped: WSPR TX audio stream is not ready"));
            return;
        }
        deferBeaconToNextSlot(tr("Re-armed · waiting for WSPR TX audio stream"));
        return;
    }

    // The transmitter can be taken between arming and the slot — MOX for a
    // quick voice contact, DAX, TCI, a tune. That is transient and the
    // operator has not withdrawn anything, so roll to the next slot rather
    // than discard the arming; the bounded deferral count still stops a
    // transmitter wedged on from leaving the beacon armed forever.
    //
    // Checked here as well as inside reassertBeaconChannel() because only this
    // caller knows a busy transmitter is worth waiting out, where a slice that
    // moved band is not.
    if (m_radioModel->transmitModel().isTransmitting()
        || m_radioModel->transmitModel().isTuning()) {
        if (m_beaconDeferrals >= kBeaconMaxDeferrals) {
            stopBeacon(tr("Stopped: transmitter is in use"));
            return;
        }
        deferBeaconToNextSlot(tr("Re-armed · transmitter is in use"));
        return;
    }

    // Last look at the channel before the key. See reassertBeaconChannel().
    QString channelProblem;
    if (!reassertBeaconChannel(&channelProblem)) {
        stopBeacon(tr("Stopped: %1").arg(channelProblem));
        return;
    }

    // Reference the lead-in to the SLOT, not to whenever this tick happened to
    // run. WSPR audio starts 1.000 s into the even minute; WSJT-X computes its
    // silent lead-in as `(delay_ms - mstr) * frameRate / 1000` against the wall
    // clock (Modulator.cpp) rather than assuming it was called on time.
    //
    // This used to pass a flat kPreRollFrames — one second measured from
    // whenever the pump started — so the 50 ms tick granularity and everything
    // behind it landed in DT instead of being absorbed. Past the 1 s mark the
    // lead-in is gone and the remainder truncates the head of the frame, again
    // as WSJT-X does, which holds DT near zero rather than sliding a 111.6 s
    // transmission later into a two-minute slot.
    //
    // What is still outside the measurement is the queued hop to the audio
    // thread, where the pump clock actually starts. That is sub-millisecond
    // against a lateness budget of two seconds.
    const qint64 lateMs = std::max<qint64>(0, nowMs - m_beaconSlotMs);
    const qint64 leadInMs = kBeaconAudioStartMs - lateMs;
    const int preRollFrames = leadInMs > 0
        ? static_cast<int>(leadInMs * WsprBeacon::kSampleRate / 1000)
        : 0;
    const int skipFrames = leadInMs < 0
        ? static_cast<int>(-leadInMs * WsprBeacon::kSampleRate / 1000)
        : 0;

    // Arm the independent DAX generator before keying. The generated-silence
    // lead-in also gives the radio's MOX edge, and the mode/filter re-assert
    // above, time to settle before the first symbol.
    beacon->start(encoded.symbols, m_beaconTone->value(),
                  static_cast<float>(m_beaconLevel->value()),
                  preRollFrames, skipFrames);
    QMetaObject::invokeMethod(
        m_audioEngine, &AudioEngine::startWsprPump, Qt::QueuedConnection);
    m_beaconStopDeadlineMs = QDateTime::currentMSecsSinceEpoch() + 115000;
    TransmitModel& tx = m_radioModel->transmitModel();
    tx.requestPttOn(TransmitModel::PttSource::Wspr);
    if (!tx.isTransmitting()) {
        beacon->stop();
        stopBeacon(tr("Transmit request was blocked"));
        return;
    }
    m_beaconTransmitting = true;
    m_beaconButton->setText(tr("Stop"));
    m_beaconStatus->setText(tr("Transmitting · pre-roll"));
}

// Where "home" is on the map. Without it the MapView has no origin, so it can
// draw the received spots (each carries its own coordinates) but no PATHS —
// which is exactly what an HL2 operator saw: a map full of spots and not one
// line joining them to anything.
//
// Both original sources were FlexRadio GPSDO readings. A radio with no GPS
// reports neither, MaidenheadLocator::toLatLon("") failed, and this returned
// having set no home position at all.
//
// The operator's own grid is the missing third source. It is the same locator
// the WSPR beacon encodes and transmits, it is now persisted, and a 4-character
// square is about 70 x 100 km — coarse for a fix and entirely adequate for
// drawing a path across a continent.
void PskReporterMapDialog::updateHomeFromRadio()
{
    double lat = 0.0, lon = 0.0;
    bool located = false;

    if (m_radioModel != nullptr) {
        // The 6000-series GPSDO reports "N 33 33.484" (hemisphere/deg/decimal-
        // minutes), not decimal degrees; parseGpsCoordinate accepts both forms
        // (#3994) — plain toDouble() here would fail and drop to the coarser grid.
        const bool ok = aprs::parseGpsCoordinate(m_radioModel->gpsLat(), lat)
                        && aprs::parseGpsCoordinate(m_radioModel->gpsLon(), lon);
        located = ok && !(lat == 0.0 && lon == 0.0);
        // Then the GPSDO-derived grid, still radio-sourced.
        if (!located) {
            located = MaidenheadLocator::toLatLon(m_radioModel->gpsGrid(), lat, lon);
        }
    }

    // Finally the operator's own grid — the beacon field, or what was persisted
    // from it. Works with no radio connected at all, which is why the whole
    // function no longer bails out on a null model: the map is a network view
    // and is perfectly usable before a radio is up.
    if (!located) {
        QString grid = m_beaconGrid != nullptr ? m_beaconGrid->text().trimmed()
                                               : QString();
        if (grid.isEmpty()) {
            grid = pskSettings().value("beaconGrid").toString().trimmed();
        }
        located = MaidenheadLocator::toLatLon(grid, lat, lon);
    }

    if (!located) {
        return;
    }
    const QString label =
        m_radioModel != nullptr ? m_radioModel->callsign() : QString();
    m_mapView->setHomePosition(lat, lon, label);
}

void PskReporterMapDialog::onIntervalChanged(int index)
{
    const int intervalMs = m_intervalCombo->itemData(index).toInt();
    writePskSetting("updateIntervalMs", intervalMs);
    restartClient();
}

void PskReporterMapDialog::onLookbackChanged(int index)
{
    // Persist immediately; defer the (networked) client update so rapid
    // toggling coalesces into one query.
    writePskSetting("lookbackSec", m_lookbackCombo->itemData(index).toInt());
    m_lookbackDebounce->start();
}

void PskReporterMapDialog::restartClient()
{
    m_client->setCallsign(m_radioModel != nullptr ? m_radioModel->callsign()
                                                  : QString());
    m_client->setLookbackSeconds(m_lookbackCombo->currentData().toInt());
    m_client->start(m_intervalCombo->currentData().toInt());
    m_emptyStateTimer->start();
}

void PskReporterMapDialog::updateConnectionIndicator()
{
    // Three-state bullet next to the transport label (MQTT/HTTP):
    //   green  = connected and showing data
    //   yellow = connected but no spots yet
    //   red    = no good connection
    const bool up = m_client->isMqttConnected() || m_client->lastHttpOk();
    const bool hasData = !m_client->spots().isEmpty();
    QString color;
    if (!up) {
        color = m_client->sawError() ? QStringLiteral("#e74c3c")    // red
                                     : QStringLiteral("#f4c20d");   // connecting
    } else {
        color = hasData ? QStringLiteral("#2ecc71")                 // green
                        : QStringLiteral("#f4c20d");                // yellow
    }
    // Transport word in the normal label color (white in dark themes);
    // only the bullet carries the status color.
    m_connLabel->setText(
        QStringLiteral("%1 <span style='color:%2;'>&#9679;</span>")
            .arg(m_client->transport(), color));
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
    const bool hasHome = m_mapView->hasHomePosition();
    // The client retains the deepest window it has fetched; filter the
    // *display* to the currently selected lookback.
    const qint64 cutoff = QDateTime::currentSecsSinceEpoch()
                          - m_client->lookbackSeconds();

    QSet<QString> bandsHeard;
    double bestKm = -1.0;
    QString farthestCall;

    for (const PskReporterSpot& spot : m_client->spots()) {
        if (spot.flowStartSeconds < cutoff) {
            continue;
        }
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
            spot, hasHome, m_mapView->homeLat(), m_mapView->homeLon(),
            lat, lon);
        m.tooltip = card;
        m.clickInfo = card;
        markers.append(m);

        bandsHeard.insert(bandName(spot.frequencyHz));
        if (hasHome) {
            const double km = MaidenheadLocator::distanceKm(
                m_mapView->homeLat(), m_mapView->homeLon(), lat, lon);
            if (km > bestKm) {
                bestKm = km;
                farthestCall = spot.receiverCallsign;
            }
        }
    }
    m_mapView->setMarkers(markers);

    // Reception stats (top-right): count · bands · farthest.
    QStringList parts;
    if (!markers.isEmpty()) {
        parts << tr("%n spot(s)", nullptr, markers.size());
        parts << tr("%n band(s)", nullptr, bandsHeard.size());
        if (bestKm >= 0.0) {
            parts << tr("farthest %1 %L2 km").arg(farthestCall).arg(qRound(bestKm));
        }
    }
    m_dxLabel->setText(parts.join(QStringLiteral("  •  ")));

    // Data presence affects the connection bullet color.
    updateConnectionIndicator();
}

void PskReporterMapDialog::updateBandConditions()
{
    if (m_propForecast == nullptr || m_bandCondPills[0] == nullptr) {
        return;
    }
    const PropForecastDetail det = m_propForecast->lastDetail();
    // Pick the day vs night rating set by the operator's local time.
    const int hour = QDateTime::currentDateTime().time().hour();
    const bool daytime = hour >= 6 && hour < 18;
    const QString tod = daytime ? tr("day") : tr("night");
    for (int i = 0; i < 4; ++i) {
        const QString cond = daytime ? det.bandDay[i] : det.bandNight[i];
        QLabel* pill = m_bandCondPills[i];
        const QString shown = cond.isEmpty() ? QStringLiteral("–") : cond;
        pill->setToolTip(tr("%1 (%2): %3")
                             .arg(QString::fromLatin1(kBandGroupLabels[i]), tod,
                                  cond.isEmpty() ? tr("no data") : cond));
        pill->setText(QStringLiteral("%1 %2")
                          .arg(QString::fromLatin1(kBandGroupLabels[i]), shown));
        pill->setStyleSheet(
            QStringLiteral("background-color: %1; color: #1a1a1a;"
                           " border-radius: 4px; padding: 1px 6px;")
                .arg(bandConditionColor(cond)));
    }
}

void PskReporterMapDialog::showEvent(QShowEvent* event)
{
    PersistentDialog::showEvent(event);
    updateHomeFromRadio();
    if (m_propForecast != nullptr) {
        // Refresh the detailed forecast (band conditions) on open; the
        // client guards against overlapping in-flight requests.
        m_propForecast->fetchDetail();
        updateBandConditions();
    }
    if (!m_started) {
        m_started = true;
        restartClient();
    }
}

void PskReporterMapDialog::closeEvent(QCloseEvent* event)
{
    if (m_beaconArmed || m_beaconTransmitting) {
        stopBeacon(tr("Stopped"));
    }
    // Stop hitting the network while the window is closed.
    m_client->stop();
    m_started = false;
    PersistentDialog::closeEvent(event);
}

} // namespace AetherSDR
