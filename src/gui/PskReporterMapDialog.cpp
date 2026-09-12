#include "PskReporterMapDialog.h"
#include "GuardedSlider.h"
#include "ComboStyle.h"

#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/aprs/AprsPacket.h"
#include "core/LogManager.h"
#include "core/MaidenheadLocator.h"
#include "core/PropForecastClient.h"
#include "core/PskReporterClient.h"
#include "core/TxKeyingMarker.h"
#include "core/WsprBeacon.h"
#include "map/CityLightsShading.h"
#include "map/MapDisplayWidget.h"
#include "map/WeatherRadarFrameTime.h"
#include "models/EqualizerModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <algorithm>
#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QFormLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QCoreApplication>
#include <QWheelEvent>
#include <QSplitter>
#include <QGroupBox>
#include <QHash>
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
#include <QToolButton>
#include <QtMath>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <limits>

namespace AetherSDR {

namespace {

// In the sidebar, wheel gestures navigate even when a value control has
// focus. Values remain editable with clicks, dragging, and the keyboard.
class SidebarValueWheelGuard final : public QObject {
public:
    explicit SidebarValueWheelGuard(QScrollArea* sidebar)
        : QObject(sidebar), m_sidebar(sidebar) {}

    void guard(QWidget* control)
    {
        control->installEventFilter(this);
        for (QWidget* child : control->findChildren<QWidget*>()) {
            child->installEventFilter(this);
        }
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() != QEvent::Wheel) {
            return QObject::eventFilter(watched, event);
        }
        auto* wheel = static_cast<QWheelEvent*>(event);
        QScrollBar* bar = m_sidebar->verticalScrollBar();
        QWheelEvent forwarded(bar->mapFromGlobal(wheel->globalPosition().toPoint()),
                              wheel->globalPosition(), wheel->pixelDelta(),
                              wheel->angleDelta(), wheel->buttons(), wheel->modifiers(),
                              wheel->phase(), wheel->inverted(), wheel->source(),
                              wheel->pointingDevice());
        QCoreApplication::sendEvent(bar, &forwarded);
        wheel->accept();
        return true;
    }

private:
    QScrollArea* m_sidebar;
};

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

    // Line 1 — both endpoints. Generic callsign searches include sent and
    // received reports, so receiver-only wording would be ambiguous.
    html += QStringLiteral("<b>%1</b> → <b>%2</b>&nbsp;&nbsp;"
                           "<span style='color:%3;'>%4</span>")
                .arg(spot.senderCallsign.toHtmlEscaped(),
                     spot.receiverCallsign.toHtmlEscaped(),
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
    , m_globalClient(new PskReporterClient(this))
    , m_propForecast(propForecast)
{
    // The callsign layer is MQTT-only. The independent all-stations client
    // owns the shared HTTP cadence and its reusable global cache.
    m_client->setHttpPollingEnabled(false);
    setMinimumSize(720, 480);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    auto* reportsBox = new QGroupBox(tr("Reports"), bodyWidget());
    reportsBox->setAccessibleName(tr("PSK Reporter filters"));

    m_queryCallsign = new QLineEdit(reportsBox);
    m_queryCallsign->setMaxLength(32);
    m_queryCallsign->setObjectName(QStringLiteral("pskReporterCallsign"));
    m_queryCallsign->setAccessibleName(tr("PSK Reporter map callsign"));
    m_queryCallsign->setAccessibleDescription(
        tr("Enter a callsign to add its live sent and received reports"));
    m_queryCallsign->setToolTip(
        tr("A callsign receives immediate live MQTT updates. Clear the field "
           "to hide the live callsign layer; All Callsigns is independent."));
    // Seed the map-only filter from the station identity. Subsequent edits do
    // not write back to the radio, and clearing it explicitly selects the
    // all-stations view.
    m_queryCallsign->setText(
        m_radioModel != nullptr
            ? m_radioModel->callsign().trimmed().toUpper().left(32)
            : QString());

    m_allCallsignsCheck = new QCheckBox(tr("All Callsigns"), reportsBox);
    m_allCallsignsCheck->setObjectName(
        QStringLiteral("pskReporterAllCallsigns"));
    m_allCallsignsCheck->setAccessibleName(
        tr("Show all PSK Reporter callsigns"));
    m_allCallsignsCheck->setToolTip(
        tr("Show the cached global HTTP snapshot and refresh it no more than "
           "once every five minutes"));
    m_allCallsignsCheck->setChecked(
        pskSettings().value("showAllCallsigns").toBool(false));
    m_bandCombo = new GuardedComboBox(reportsBox);
    m_bandCombo->addItem(tr("All"));
    for (const char* b : { "160m", "80m", "60m", "40m", "30m", "20m",
                           "17m", "15m", "12m", "10m", "6m", "VHF+" }) {
        m_bandCombo->addItem(QString::fromLatin1(b));
    }

    m_modeCombo = new GuardedComboBox(reportsBox);
    m_modeCombo->addItem(tr("All"));
    for (const char* m : { "FT8", "FT4", "WSPR", "JS8", "CW", "PSK",
                           "RTTY", "SSB", "Other" }) {
        m_modeCombo->addItem(QString::fromLatin1(m));
    }

    m_lookbackCombo = new GuardedComboBox(reportsBox);
    m_lookbackCombo->addItem(tr("15 min"), 15 * 60);
    m_lookbackCombo->addItem(tr("30 min"), 30 * 60);
    m_lookbackCombo->addItem(tr("1 hour"), 60 * 60);
    m_lookbackCombo->addItem(tr("2 hours"), 2 * 60 * 60);
    m_lookbackCombo->addItem(tr("4 hours"), 4 * 60 * 60);
    m_lookbackCombo->addItem(tr("8 hours"), 8 * 60 * 60);

    m_dxLabel = new QLabel(reportsBox);
    m_dxLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);


    m_activeMonitorsCheck = new QCheckBox(tr("Active monitors"), reportsBox);
    m_activeMonitorsCheck->setObjectName(
        QStringLiteral("pskReporterActiveMonitors"));
    m_activeMonitorsCheck->setAccessibleName(
        tr("Show active PSK Reporter monitors"));
    m_activeMonitorsCheck->setToolTip(
        tr("Show stations that report themselves as listening; these records "
           "do not contain a transmitting endpoint, so they have no paths"));
    m_activeMonitorsCheck->setChecked(
        pskSettings().value("showActiveMonitors").toBool(true));


    m_globeCheck = new QCheckBox(tr("Globe"), reportsBox);
    m_globeCheck->setObjectName(QStringLiteral("pskReporterGlobeToggle"));
    m_globeCheck->setAccessibleName(tr("Show PSK Reporter as a globe"));
    m_globeCheck->setToolTip(
        tr("Switch between the flat world map and an interactive globe"));
    m_globeCheck->setChecked(
        pskSettings().value("showGlobe").toBool(false));

    m_pathsCheck = new QCheckBox(tr("Paths"), reportsBox);
    m_pathsCheck->setObjectName(QStringLiteral("pskReporterPaths"));
    m_pathsCheck->setToolTip(
        tr("Draw great-circle report paths between transmitting and receiving stations"));
    m_pathsCheck->setChecked(pskSettings().value("showPaths").toBool(false));

    m_terminatorCheck = new QCheckBox(tr("Day/night"), reportsBox);
    m_terminatorCheck->setToolTip(tr("Show the current night shadow on the map"));
    m_terminatorCheck->setAccessibleName(tr("Show day and night terminator"));
    m_terminatorCheck->setChecked(
        pskSettings().value("showTerminator").toBool(true));

    m_cityLightsCheck = new QCheckBox(tr("City lights"), reportsBox);
    m_cityLightsCheck->setObjectName(QStringLiteral("pskReporterCityLights"));
    m_cityLightsCheck->setAccessibleName(tr("Show NASA city lights"));
    m_cityLightsCheck->setAccessibleDescription(tr(
        "Historical NASA/GSFC night lights from 2016, not current activity. "
        "With Day/night enabled, lights fade in during twilight. "
        "Otherwise lights are visible worldwide."));
    m_cityLightsCheck->setToolTip(m_cityLightsCheck->accessibleDescription());
    m_cityLightsCheck->setChecked(pskSettings().value("showCityLights").toBool(false));

    m_weatherRadarCheck = new QCheckBox(tr("Weather overlay"), reportsBox);
    m_weatherRadarCheck->setObjectName(
        QStringLiteral("pskReporterWeatherRadar"));
    m_weatherRadarCheck->setAccessibleName(
        tr("Show weather precipitation overlay"));
    m_weatherRadarCheck->setAccessibleDescription(tr(
        "Shows LibreWXR radar and satellite/model precipitation estimates, "
        "with enabled regional radar backups when unavailable."));
    m_weatherRadarCheck->setToolTip(tr(
        "Overlay near-real-time weather radar; disabled "
        "when this checkbox is off"));
    m_weatherRadarCheck->setChecked(
        pskSettings().value("showWeatherRadar").toBool(false));

    const QStringList regionNames{tr("US backup · NOAA"), tr("Canada backup · ECCC"), tr("Europe backup · OPERA"), tr("Global primary · LibreWXR")};
    const QStringList regionIds{QStringLiteral("pskReporterRadarUS"), QStringLiteral("pskReporterRadarCanada"), QStringLiteral("pskReporterRadarEurope"), QStringLiteral("pskReporterRadarGlobal")};
    const int savedRegions = pskSettings().value("weatherRadarRegions").toInt(7) & 7;
    for (int i = 0; i < 4; ++i) {
        m_radarRegionChecks[i] = new QCheckBox(regionNames[i], reportsBox);
        m_radarRegionChecks[i]->setObjectName(regionIds[i]);
        m_radarRegionChecks[i]->setAccessibleName(regionNames[i]);
        m_radarRegionChecks[i]->setChecked(i == 3 ? pskSettings().value("useLibreWxr").toBool(true) : (savedRegions & (1 << i)) != 0);
        m_radarRegionChecks[i]->setToolTip(i == 3
            ? tr("Free global precipitation from LibreWXR. Includes radar, satellite estimates and model data. Disable to use the regional radar feeds directly.")
            : tr("Use this regional radar feed when LibreWXR is disabled or unavailable."));
    }
    m_radarProductLabel = new QLabel(reportsBox);
    m_radarProductLabel->setObjectName(QStringLiteral("pskReporterRadarProduct"));
    m_radarProductLabel->setAccessibleName(tr("Radar product and units"));
    m_radarProductLabel->setWordWrap(true);
    m_radarCoverageCheck = new QCheckBox(tr("Radar coverage"), reportsBox);
    m_radarCoverageCheck->setObjectName(QStringLiteral("pskReporterRadarCoverage"));
    m_radarCoverageCheck->setAccessibleName(tr("Show radar sites and nominal coverage"));
    m_radarCoverageCheck->setAccessibleDescription(tr(
        "Faint nominal coverage shading for sites with published ranges. "
        "Terrain, outages and scanning conditions reduce actual coverage."));
    m_radarCoverageCheck->setToolTip(m_radarCoverageCheck->accessibleDescription());
    m_radarCoverageCheck->setChecked(pskSettings().value("showRadarCoverage").toBool(false));

    m_weatherRadarPlayButton = new QToolButton(reportsBox);
    m_weatherRadarPlayButton->setObjectName(
        QStringLiteral("pskReporterWeatherRadarPlay"));
    m_weatherRadarPlayButton->setText(QStringLiteral("▶"));
    m_weatherRadarPlayButton->setAutoRaise(true);
    m_weatherRadarPlayButton->setAccessibleName(
        tr("Play historical weather radar"));
    m_weatherRadarPlayButton->setToolTip(
        tr("Loop through published radar images; no generated transitions"));
    m_weatherRadarPlayButton->setEnabled(
        m_weatherRadarCheck->isChecked());

    m_weatherRadarHistoryCombo = new GuardedComboBox(reportsBox);
    m_weatherRadarHistoryCombo->setObjectName(
        QStringLiteral("pskReporterWeatherRadarHistory"));
    m_weatherRadarHistoryCombo->setAccessibleName(
        tr("Weather radar history duration"));
    m_weatherRadarHistoryCombo->addItem(tr("1 h"), 1);
    m_weatherRadarHistoryCombo->addItem(tr("2 h"), 2);
    m_weatherRadarHistoryCombo->addItem(tr("4 h"), 4);
    const int savedRadarHours = std::clamp(
        pskSettings().value("weatherRadarHistoryHours").toInt(1), 1, 4);
    const int savedRadarHoursIndex =
        m_weatherRadarHistoryCombo->findData(savedRadarHours);
    m_weatherRadarHistoryCombo->setCurrentIndex(
        savedRadarHoursIndex >= 0 ? savedRadarHoursIndex : 0);
    m_weatherRadarHistoryCombo->setEnabled(
        m_weatherRadarCheck->isChecked());

    auto* radarSpeedLabel = new QLabel(tr("Speed:"), reportsBox);
    m_weatherRadarSpeedSlider = new GuardedSlider(Qt::Horizontal, reportsBox);
    m_weatherRadarSpeedSlider->setObjectName(QStringLiteral("pskReporterWeatherRadarSpeed"));
    m_weatherRadarSpeedSlider->setAccessibleName(tr("Weather radar playback speed"));
    m_weatherRadarSpeedSlider->setAccessibleDescription(tr(
        "25 to 500 percent of normal speed (0.25 to 5 times). "
        "Changes how quickly original radar images loop without reloading them. "
        "The final image always holds for one second."));
    m_weatherRadarSpeedSlider->setToolTip(m_weatherRadarSpeedSlider->accessibleDescription());
    m_weatherRadarSpeedSlider->setRange(25, 500);
    m_weatherRadarSpeedSlider->setSingleStep(1);
    m_weatherRadarSpeedSlider->setPageStep(25);
    m_weatherRadarSpeedSlider->setFocusPolicy(Qt::StrongFocus);
    m_weatherRadarSpeedSlider->setDragValueFormatter([](int speed) {
        return QStringLiteral("%1×").arg(speed / 100.0, 0, 'f', 2);
    });
    applyPrimarySliderStyle(m_weatherRadarSpeedSlider);
    const int savedRadarSpeed = weatherRadarPlaybackSpeedPercent(
        pskSettings().value("weatherRadarSpeedPercent").toInt(100));
    m_weatherRadarSpeedSlider->setValue(savedRadarSpeed);
    m_weatherRadarSpeedSlider->setEnabled(m_weatherRadarCheck->isChecked());
    m_weatherRadarSpeedValue = new QLabel(
        QStringLiteral("%1×").arg(savedRadarSpeed / 100.0, 0, 'f', 2), reportsBox);
    m_weatherRadarSpeedValue->setObjectName(QStringLiteral("pskReporterWeatherRadarSpeedValue"));
    m_weatherRadarSpeedValue->setMinimumWidth(42);
    radarSpeedLabel->setBuddy(m_weatherRadarSpeedSlider);
    m_weatherRadarFrameLabel = new QLabel(tr("Age unknown"), reportsBox);
    m_weatherRadarFrameLabel->setObjectName(
        QStringLiteral("pskReporterWeatherRadarFrame"));
    m_weatherRadarFrameLabel->setAccessibleName(
        tr("Weather radar frame time"));
    m_weatherRadarFrameLabel->setMinimumWidth(135);
    m_weatherRadarFrameLabel->setEnabled(
        m_weatherRadarCheck->isChecked());
    auto* lightsLabel = new QLabel(tr("Brightness:"), reportsBox);
    m_cityLightsBrightness = new GuardedSlider(Qt::Horizontal, reportsBox);
    m_cityLightsBrightness->setObjectName(QStringLiteral("pskReporterCityLightsBrightness"));
    m_cityLightsBrightness->setAccessibleName(tr("City lights brightness"));
    m_cityLightsBrightness->setAccessibleDescription(tr("Overlay intensity from 0 to 100 percent."));
    m_cityLightsBrightness->setToolTip(m_cityLightsBrightness->accessibleDescription());
    m_cityLightsBrightness->setRange(0, 100);
    m_cityLightsBrightness->setFocusPolicy(Qt::StrongFocus);
    m_cityLightsBrightness->setValue(std::clamp(
        pskSettings().value("cityLightsBrightness").toInt(CityLightsShading::kDefaultBrightness), 0, 100));
    m_cityLightsBrightness->setDragValueFormatter([](int value) {
        return QStringLiteral("%1%").arg(value);
    });
    applyPrimarySliderStyle(m_cityLightsBrightness);
    lightsLabel->setBuddy(m_cityLightsBrightness);
    auto* lightsValue = new QLabel(QStringLiteral("%1%").arg(m_cityLightsBrightness->value()), reportsBox);
    lightsValue->setMinimumWidth(36);
    auto* faintLabel = new QLabel(tr("Faint lights:"), reportsBox);
    m_cityLightsFaintLights = new GuardedSlider(Qt::Horizontal, reportsBox);
    m_cityLightsFaintLights->setObjectName(QStringLiteral("pskReporterCityLightsFaintLights"));
    m_cityLightsFaintLights->setAccessibleName(tr("City lights faint lights"));
    m_cityLightsFaintLights->setAccessibleDescription(tr(
        "Reveal dim settlements without washing out bright city centers. Zero preserves the original intensity; 100 gives the strongest enhancement."));
    m_cityLightsFaintLights->setToolTip(m_cityLightsFaintLights->accessibleDescription());
    m_cityLightsFaintLights->setRange(0, 100);
    m_cityLightsFaintLights->setFocusPolicy(Qt::StrongFocus);
    m_cityLightsFaintLights->setValue(std::clamp(
        pskSettings().value("cityLightsFaintLights").toInt(CityLightsShading::kDefaultFaintLights), 0, 100));
    m_cityLightsFaintLights->setDragValueFormatter([](int value) {
        return QStringLiteral("%1%").arg(value);
    });
    applyPrimarySliderStyle(m_cityLightsFaintLights);
    faintLabel->setBuddy(m_cityLightsFaintLights);
    auto* faintValue = new QLabel(QStringLiteral("%1%").arg(m_cityLightsFaintLights->value()), reportsBox);
    faintValue->setMinimumWidth(36);
    connect(m_cityLightsFaintLights, &QSlider::valueChanged, faintValue, [faintValue](int value) {
        faintValue->setText(QStringLiteral("%1%").arg(value));
    });
    auto* warmthLabel = new QLabel(tr("Warmth:"), reportsBox);
    m_cityLightsWarmth = new GuardedSlider(Qt::Horizontal, reportsBox);
    m_cityLightsWarmth->setObjectName(QStringLiteral("pskReporterCityLightsWarmth"));
    m_cityLightsWarmth->setAccessibleName(tr("City lights warmth"));
    m_cityLightsWarmth->setAccessibleDescription(tr(
        "Adjust the display tint from original white at 0 to warm golden light at 100. This is a visual preference, not measured lamp color."));
    m_cityLightsWarmth->setToolTip(m_cityLightsWarmth->accessibleDescription());
    m_cityLightsWarmth->setRange(0, 100);
    m_cityLightsWarmth->setFocusPolicy(Qt::StrongFocus);
    m_cityLightsWarmth->setValue(std::clamp(
        pskSettings().value("cityLightsWarmth").toInt(CityLightsShading::kDefaultWarmth), 0, 100));
    m_cityLightsWarmth->setDragValueFormatter([](int value) {
        return QStringLiteral("%1%").arg(value);
    });
    applyPrimarySliderStyle(m_cityLightsWarmth);
    warmthLabel->setBuddy(m_cityLightsWarmth);
    auto* warmthValue = new QLabel(QStringLiteral("%1%").arg(m_cityLightsWarmth->value()), reportsBox);
    warmthValue->setMinimumWidth(36);
    connect(m_cityLightsWarmth, &QSlider::valueChanged, warmthValue, [warmthValue](int value) {
        warmthValue->setText(QStringLiteral("%1%").arg(value));
    });
    for (QWidget* widget : QList<QWidget*>{lightsLabel, m_cityLightsBrightness,
                                          lightsValue, faintLabel, m_cityLightsFaintLights, faintValue, warmthLabel, m_cityLightsWarmth, warmthValue}) {
        widget->setVisible(m_cityLightsCheck->isChecked());
        connect(m_cityLightsCheck, &QCheckBox::toggled, widget, &QWidget::setVisible);
    }
    connect(m_cityLightsBrightness, &QSlider::valueChanged, lightsValue, [lightsValue](int value) {
        lightsValue->setText(QStringLiteral("%1%").arg(value));
    });

    auto* beaconBox = new QGroupBox(tr("WSPR beacon"), bodyWidget());
    beaconBox->setAccessibleName(tr("WSPR beacon transmitter"));

    m_beaconCallsign = new QLineEdit(beaconBox);
    m_beaconCallsign->setMaxLength(6);
    m_beaconCallsign->setAccessibleName(tr("WSPR callsign"));
    m_beaconCallsign->setAccessibleDescription(
        tr("Station callsign used for WSPR transmission; changes update the station callsign"));

    m_beaconGrid = new QLineEdit(beaconBox);
    m_beaconGrid->setMaxLength(4);
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

    m_beaconBand = new GuardedComboBox(beaconBox);
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

    m_beaconPower = new GuardedComboBox(beaconBox);
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

    m_beaconButton = new QPushButton(tr("Transmit once"), beaconBox);
    m_beaconButton->setAutoDefault(false);
    markTxKeying(m_beaconButton);
    m_beaconButton->setAccessibleName(tr("Transmit one WSPR beacon"));
    m_beaconButton->setAccessibleDescription(
        tr("Arms one transmission for the next even UTC minute"));

    m_beaconStatusDot = new QLabel(beaconBox);
    m_beaconStatusDot->setObjectName(QStringLiteral("pskReporterBeaconStatusDot"));
    m_beaconStatusDot->setFixedSize(8, 8);
    m_beaconStatus = new QLabel(beaconBox);
    m_beaconStatus->setWordWrap(true);
    m_beaconStatus->setAccessibleName(tr("WSPR beacon status"));
    setBeaconStatus(tr("Idle"), "color.accent.success");

    // A scrollable control column can grow without increasing the window's
    // minimum height or taking vertical space away from either map renderer.
    auto* splitter = new QSplitter(Qt::Horizontal, bodyWidget());
    splitter->setObjectName(QStringLiteral("pskReporterSplitter"));
    splitter->setChildrenCollapsible(false);
    auto* sidebar = new QScrollArea(splitter);
    sidebar->setObjectName(QStringLiteral("pskReporterSidebar"));
    sidebar->setAccessibleName(tr("PSK Reporter controls"));
    sidebar->setWidgetResizable(true);
    sidebar->setFrameShape(QFrame::NoFrame);
    sidebar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sidebar->setMinimumWidth(270);
    auto* controls = new QWidget(sidebar);
    auto* sections = new QVBoxLayout(controls);
    sections->setContentsMargins(0, 0, 6, 0);
    sections->setSpacing(6);

    const auto formFor = [](QGroupBox* box) {
        auto* form = new QFormLayout(box);
        form->setContentsMargins(6, 6, 6, 6);
        form->setHorizontalSpacing(6);
        form->setVerticalSpacing(4);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        return form;
    };
    QFormLayout* beaconForm = formFor(beaconBox);
    beaconForm->addRow(tr("TX call:"), m_beaconCallsign);
    beaconForm->addRow(tr("Grid:"), m_beaconGrid);
    beaconForm->addRow(tr("Band:"), m_beaconBand);
    beaconForm->addRow(tr("Reported:"), m_beaconPower);
    beaconForm->addRow(tr("Offset:"), m_beaconTone);
    beaconForm->addRow(tr("Level:"), m_beaconLevel);
    auto* beaconActionRow = new QHBoxLayout();
    beaconActionRow->setSpacing(6);
    beaconActionRow->addWidget(m_beaconButton);
    beaconActionRow->addStretch(1);
    beaconActionRow->addWidget(m_beaconStatusDot, 0, Qt::AlignVCenter);
    beaconActionRow->addWidget(m_beaconStatus, 1);
    beaconForm->addRow(beaconActionRow);
    sections->addWidget(beaconBox);

    QFormLayout* reportsForm = formFor(reportsBox);
    reportsForm->addRow(tr("Call:"), m_queryCallsign);
    reportsForm->addRow(tr("Band:"), m_bandCombo);
    reportsForm->addRow(tr("Mode:"), m_modeCombo);
    reportsForm->addRow(tr("Lookback:"), m_lookbackCombo);
    reportsForm->addRow(m_allCallsignsCheck);
    reportsForm->addRow(m_activeMonitorsCheck);
    sections->addWidget(reportsBox);

    auto* mapBox = new QGroupBox(tr("Map"), controls);
    QFormLayout* mapForm = formFor(mapBox);
    mapForm->addRow(m_globeCheck);
    mapForm->addRow(m_pathsCheck);
    mapForm->addRow(m_terminatorCheck);
    auto* basemapDarkTint = new QCheckBox(tr("Dark map"), mapBox);
    basemapDarkTint->setObjectName(QStringLiteral("pskReporterBasemapDarkTint"));
    basemapDarkTint->setAccessibleName(tr("Dark basemap"));
    basemapDarkTint->setAccessibleDescription(tr(
        "Remap the map to dark backgrounds and light printed labels. "
        "City lights, radar, and reports keep their colours."));
    basemapDarkTint->setToolTip(basemapDarkTint->accessibleDescription());
    basemapDarkTint->setChecked(
        pskSettings().value("basemapDarkTintEnabled").toBool(false));
    mapForm->addRow(basemapDarkTint);
    auto* basemapBrightness = new GuardedSlider(Qt::Horizontal, mapBox);
    basemapBrightness->setObjectName(QStringLiteral("pskReporterBasemapBrightness"));
    basemapBrightness->setAccessibleName(tr("Map brightness"));
    basemapBrightness->setAccessibleDescription(tr(
        "Dim the basemap without dimming city lights, radar, or reports. "
        "This does not change the actual day/night boundary."));
    basemapBrightness->setToolTip(basemapBrightness->accessibleDescription());
    basemapBrightness->setRange(20, 100);
    basemapBrightness->setFocusPolicy(Qt::StrongFocus);
    basemapBrightness->setValue(std::clamp(
        pskSettings().value("basemapBrightness").toInt(100), 20, 100));
    auto* basemapValue = new QLabel(tr("%1%").arg(basemapBrightness->value()), mapBox);
    auto* basemapRow = new QHBoxLayout();
    basemapRow->addWidget(basemapBrightness, 1);
    basemapRow->addWidget(basemapValue);
    auto* basemapLabel = new QLabel(tr("Map brightness:"), mapBox);
    basemapLabel->setBuddy(basemapBrightness);
    mapForm->addRow(basemapLabel, basemapRow);
    sections->addWidget(mapBox);

    const auto sliderRow = [](QSlider* slider, QLabel* value) {
        auto* row = new QHBoxLayout();
        row->setSpacing(4);
        slider->setMinimumWidth(70);
        row->addWidget(slider, 1);
        row->addWidget(value);
        return row;
    };
    auto* lightsBox = new QGroupBox(tr("City lights"), controls);
    QFormLayout* lightsForm = formFor(lightsBox);
    lightsForm->addRow(m_cityLightsCheck);
    lightsForm->addRow(lightsLabel, sliderRow(m_cityLightsBrightness, lightsValue));
    lightsForm->addRow(faintLabel, sliderRow(m_cityLightsFaintLights, faintValue));
    lightsForm->addRow(warmthLabel, sliderRow(m_cityLightsWarmth, warmthValue));
    sections->addWidget(lightsBox);

    auto* radarBox = new QGroupBox(tr("Weather precipitation"), controls);
    QFormLayout* radarForm = formFor(radarBox);
    radarForm->addRow(m_weatherRadarCheck);
    radarForm->addRow(m_radarRegionChecks[3]);
    for (int i = 0; i < 3; ++i) { radarForm->addRow(m_radarRegionChecks[i]); }
    auto* radarInfoButton = new QToolButton(radarBox);
    radarInfoButton->setObjectName(QStringLiteral("pskReporterRadarInfo"));
    radarInfoButton->setText(tr("Sources && licenses"));
    radarInfoButton->setAccessibleName(tr("Weather sources, licenses and product details"));
    radarInfoButton->setCheckable(true);
    radarInfoButton->setAutoRaise(true);
    radarInfoButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    radarInfoButton->setArrowType(Qt::RightArrow);
    auto* radarInfo = new QWidget(radarBox);
    radarInfo->setObjectName(QStringLiteral("pskReporterRadarInfoPanel"));
    auto* radarInfoLayout = new QVBoxLayout(radarInfo);
    radarInfoLayout->setContentsMargins(0, 0, 0, 0);
    radarInfoLayout->setSpacing(4);
    auto* radarCredits = new QLabel(tr("Weather data: <a href=\"https://librewxr.net/\">LibreWXR</a> · "
        "<a href=\"https://creativecommons.org/licenses/by/4.0/\">CC BY 4.0</a>. "
        "Italian Radar-DPC imagery: <a href=\"https://creativecommons.org/licenses/by-sa/4.0/\">CC BY-SA 4.0</a>. "
        "Images are reprojected, resized and composited for this map."), radarInfo);
    radarCredits->setObjectName(QStringLiteral("pskReporterRadarCredits"));
    radarCredits->setAccessibleName(tr("LibreWXR sources and licenses"));
    radarCredits->setOpenExternalLinks(true);
    radarCredits->setWordWrap(true);
    radarCredits->setToolTip(tr("Weather data via LibreWXR (librewxr.net). Sources: NOAA/NCEP/NESDIS, IEM, "
        "ECCC, EUMETNET OPERA, Radar-DPC, MARN/SNET, CWA, JMA, MET Malaysia and PAGASA. "
        "Models: NOAA, ECCC, DMI, DWD, Météo-France, SMN, JMA and ECMWF via Open-Meteo. "
        "CC BY 4.0; tiles containing Radar-DPC data are CC BY-SA 4.0. See the linked source list."));
    radarInfoLayout->addWidget(radarCredits);
    auto* globalCredits = new QLabel(tr(
        "LibreWXR contributors: NOAA/NCEP/NESDIS; Iowa Environmental Mesonet; ECCC/MSC; "
        "EUMETNET OPERA; Radar-DPC; MARN/SNET; Central Weather Administration, Taiwan; "
        "Japan Meteorological Agency; © Jabatan Meteorologi Malaysia / METMalaysia; PAGASA / DOST. "
        "Precipitation data from NOAA Enterprise Rain Rate (RRQPE). "
        "Models: NOAA, ECCC, DMI, DWD, Météo-France, SMN Argentina, JMA and ECMWF via Open-Meteo. "
        "<a href=\"https://librewxr.net/#data-sources\">Full source list and provider licenses</a>."), radarInfo);
    globalCredits->setObjectName(QStringLiteral("pskReporterGlobalCredits"));
    globalCredits->setOpenExternalLinks(true);
    globalCredits->setWordWrap(true);
    radarInfoLayout->addWidget(globalCredits);
    auto* regionalCredits = new QLabel(tr(
        "Regional backups: NOAA/NWS (public domain); "
        "<a href=\"https://open.canada.ca/en/open-government-licence-canada\">ECCC/MSC (Open Government Licence – Canada)</a>; "
        "EUMETNET OPERA (<a href=\"https://creativecommons.org/licenses/by/4.0/\">CC BY 4.0</a>)."), radarInfo);
    regionalCredits->setObjectName(QStringLiteral("pskReporterRegionalCredits"));
    regionalCredits->setOpenExternalLinks(true);
    regionalCredits->setWordWrap(true);
    radarInfoLayout->addWidget(regionalCredits);
    auto* mapCredits = new QLabel(tr(
        "Map: © <a href=\"https://www.openstreetmap.org/copyright\">OpenStreetMap contributors (ODbL)</a>. "
        "City lights: NASA/GSFC, 2016. Radar sites: NOAA/NWS and EUMETNET OPERA; "
        "shading shows nominal range, not current availability."), radarInfo);
    mapCredits->setObjectName(QStringLiteral("pskReporterMapCredits"));
    mapCredits->setOpenExternalLinks(true);
    mapCredits->setWordWrap(true);
    radarInfoLayout->addWidget(mapCredits);
    radarInfoLayout->addWidget(m_radarProductLabel);
    radarInfo->hide();
    connect(radarInfoButton, &QToolButton::toggled, radarInfo,
        [radarInfoButton, radarInfo](bool expanded) {
            radarInfoButton->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
            radarInfo->setVisible(expanded);
        });
    radarForm->addRow(radarInfoButton);
    radarForm->addRow(radarInfo);
    auto* radarStatus = new QLabel(radarBox);
    radarStatus->setObjectName(QStringLiteral("pskReporterRadarProviderStatus"));
    radarStatus->setAccessibleName(tr("Weather provider availability"));
    radarStatus->setWordWrap(true);
    radarStatus->hide();
    radarForm->addRow(radarStatus);
    radarForm->addRow(m_radarCoverageCheck);
    auto* coverageStatus = new QLabel(radarBox);
    coverageStatus->setObjectName(QStringLiteral("pskReporterRadarCoverageStatus"));
    coverageStatus->setAccessibleName(tr("Radar coverage status"));
    coverageStatus->setWordWrap(true);
    radarForm->addRow(coverageStatus);
    auto* playbackRow = new QHBoxLayout();
    playbackRow->setSpacing(4);
    playbackRow->addWidget(m_weatherRadarPlayButton);
    playbackRow->addWidget(m_weatherRadarHistoryCombo, 1);
    radarForm->addRow(tr("History:"), playbackRow);
    radarForm->addRow(radarSpeedLabel,
                      sliderRow(m_weatherRadarSpeedSlider, m_weatherRadarSpeedValue));
    radarForm->addRow(m_weatherRadarFrameLabel);
    sections->addWidget(radarBox);
    sections->addStretch(1);
    sidebar->setWidget(controls);

    auto* wheelGuard = new SidebarValueWheelGuard(sidebar);
    for (QWidget* control : QList<QWidget*>{m_beaconTone, m_beaconLevel, basemapBrightness,
             m_cityLightsBrightness, m_cityLightsFaintLights, m_cityLightsWarmth,
             m_weatherRadarSpeedSlider}) {
        wheelGuard->guard(control);
    }

    for (QComboBox* combo : {m_bandCombo, m_modeCombo, m_lookbackCombo,
                            m_beaconBand, m_beaconPower, m_weatherRadarHistoryCombo}) {
        applyComboStyle(combo);
    }
    // Creation order differs from display order; keep keyboard navigation
    // following the sidebar from the beacon down through the overlays.
    const QList<QWidget*> tabOrder = {
        m_beaconCallsign, m_beaconGrid, m_beaconBand, m_beaconPower,
        m_beaconTone, m_beaconLevel, m_beaconButton, m_queryCallsign,
        m_bandCombo, m_modeCombo, m_lookbackCombo, m_allCallsignsCheck,
        m_activeMonitorsCheck, m_globeCheck, m_pathsCheck, m_terminatorCheck, basemapDarkTint, basemapBrightness,
        m_cityLightsCheck, m_cityLightsBrightness, m_cityLightsFaintLights,
        m_cityLightsWarmth, m_weatherRadarCheck, m_radarRegionChecks[3], m_radarRegionChecks[0], m_radarRegionChecks[1], m_radarRegionChecks[2], m_radarCoverageCheck, m_weatherRadarPlayButton,
        m_weatherRadarHistoryCombo, m_weatherRadarSpeedSlider};
    for (int i = 1; i < tabOrder.size(); ++i) {
        QWidget::setTabOrder(tabOrder[i - 1], tabOrder[i]);
    }

    auto* mapPanel = new QWidget(splitter);
    mapPanel->setMinimumWidth(320);
    auto* mapLayout = new QVBoxLayout(mapPanel);
    mapLayout->setContentsMargins(0, 0, 0, 0);
    mapLayout->setSpacing(4);
    m_dxLabel->setWordWrap(true);
    m_dxLabel->hide();
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({300, 900});
    root->addWidget(splitter, 1);

    m_mapView = new MapDisplayWidget(bodyWidget());
    m_mapView->setDetailedAttributionVisible(false);
    connect(m_mapView, &MapDisplayWidget::radarCoverageStatusChanged, coverageStatus, &QLabel::setText);
    m_mapView->setBasemapDarkEnabled(basemapDarkTint->isChecked());
    connect(basemapDarkTint, &QCheckBox::toggled, this, [this](bool enabled) {
        m_mapView->setBasemapDarkEnabled(enabled);
        writePskSetting("basemapDarkTintEnabled", enabled);
    });
    m_mapView->setBasemapBrightness(basemapBrightness->value());
    connect(basemapBrightness, &QSlider::valueChanged, this,
            [this, basemapValue](int value) {
                basemapValue->setText(tr("%1%").arg(value));
                m_mapView->setBasemapBrightness(value);
                writePskSetting("basemapBrightness", value);
            });
    m_mapView->setObjectName(QStringLiteral("pskReporterMap"));
    m_mapView->setAccessibleName(tr("PSK Reporter map"));
    connect(m_mapView, &MapDisplayWidget::globeAvailabilityChanged,
            this, [this](bool available, const QString& reason) {
                if (available) {
                    return;
                }
                const QSignalBlocker blocker(m_globeCheck);
                m_globeCheck->setChecked(false);
                m_globeCheck->setEnabled(false);
                m_globeCheck->setToolTip(reason);
                m_globeCheck->setAccessibleDescription(reason);
                writePskSetting("showGlobe", false);
            });
    m_mapView->setProjectionMode(m_globeCheck->isChecked()
        ? MapDisplayWidget::ProjectionMode::Globe
        : MapDisplayWidget::ProjectionMode::Flat);
    m_mapView->setPathsVisible(m_pathsCheck->isChecked());
    m_mapView->setDayNightTerminatorVisible(m_terminatorCheck->isChecked());
    m_mapView->setCityLightsWarmth(m_cityLightsWarmth->value());
    connect(m_cityLightsWarmth, &QSlider::valueChanged, this, [this](int percent) {
        writePskSetting("cityLightsWarmth", percent);
        m_mapView->setCityLightsWarmth(percent);
    });
    m_mapView->setCityLightsFaintLights(m_cityLightsFaintLights->value());
    connect(m_cityLightsFaintLights, &QSlider::valueChanged, this, [this](int percent) {
        writePskSetting("cityLightsFaintLights", percent);
        m_mapView->setCityLightsFaintLights(percent);
    });
    m_mapView->setCityLightsBrightness(m_cityLightsBrightness->value());
    m_mapView->setCityLightsVisible(m_cityLightsCheck->isChecked());
    connect(m_cityLightsCheck, &QCheckBox::toggled, this, [this](bool on) {
        writePskSetting("showCityLights", on);
        m_mapView->setCityLightsVisible(on);
    });
    connect(m_cityLightsBrightness, &QSlider::valueChanged, this, [this](int percent) {
        writePskSetting("cityLightsBrightness", percent);
        m_mapView->setCityLightsBrightness(percent);
    });
    mapLayout->addWidget(m_mapView, 1);

    connect(m_pathsCheck, &QCheckBox::toggled, this, [this](bool on) {
        writePskSetting("showPaths", on);
        m_mapView->setPathsVisible(on);
    });
    auto updateRadarRegions = [this] {
        int mask = 0;
        for (int i = 0; i < 4; ++i) { if (m_radarRegionChecks[i]->isChecked()) { mask |= 1 << i; } }
        m_mapView->setWeatherRadarRegions(mask);
        m_radarProductLabel->setText(WeatherRadarSource::composite(mask).productDescription());
        m_weatherRadarPlayButton->setEnabled(mask != 0 && m_weatherRadarCheck->isChecked());
        writePskSetting("weatherRadarRegions", mask & 7);
        writePskSetting("useLibreWxr", (mask & 8) != 0);
    };
    for (QCheckBox* region : m_radarRegionChecks) {
        connect(region, &QCheckBox::toggled, this, updateRadarRegions);
    }
    updateRadarRegions();
    connect(m_mapView, &MapDisplayWidget::radarProviderStatusChanged, radarStatus, [radarStatus](const QString& status) {
        radarStatus->setText(status);
        radarStatus->setVisible(!status.isEmpty());
    });
    connect(m_radarCoverageCheck, &QCheckBox::toggled, this, [this](bool on) {
        writePskSetting("showRadarCoverage", on);
        m_mapView->setRadarCoverageVisible(on && isVisible());
    });
    connect(m_weatherRadarCheck, &QCheckBox::toggled, this,
            [this](bool on) {
                writePskSetting("showWeatherRadar", on);
                m_weatherRadarPlayButton->setEnabled(on && (m_radarRegionChecks[0]->isChecked()
                    || m_radarRegionChecks[1]->isChecked() || m_radarRegionChecks[2]->isChecked() || m_radarRegionChecks[3]->isChecked()));
                m_weatherRadarHistoryCombo->setEnabled(on);
                m_weatherRadarSpeedSlider->setEnabled(on);
                m_weatherRadarFrameLabel->setEnabled(on);
                if (!on) {
                    m_weatherRadarTimelineLoading = false;
                    m_weatherRadarPlayButton->setText(
                        QStringLiteral("▶"));
                    const WeatherRadarFramePresentation presentation =
                        weatherRadarFramePresentation({}, true);
                    m_weatherRadarFrameLabel->setText(presentation.text);
                    m_weatherRadarFrameLabel->setToolTip(presentation.tooltip);
                }
                if (isVisible()) {
                    m_mapView->setWeatherRadarVisible(on);
                }
            });
    connect(m_weatherRadarPlayButton, &QToolButton::clicked, this,
            [this] {
                if (m_mapView->weatherRadarAnimating()
                    || m_weatherRadarTimelineLoading) {
                    m_mapView->stopWeatherRadarAnimation();
                    return;
                }
                m_mapView->startWeatherRadarAnimation(
                    m_weatherRadarHistoryCombo->currentData().toInt());
            });
    connect(m_weatherRadarHistoryCombo,
            qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) {
                const int hours =
                    m_weatherRadarHistoryCombo->currentData().toInt();
                writePskSetting("weatherRadarHistoryHours", hours);
                if (m_mapView->weatherRadarAnimating()
                    || m_weatherRadarTimelineLoading) {
                    m_mapView->startWeatherRadarAnimation(hours);
                }
            });
    connect(m_weatherRadarSpeedSlider, &QSlider::valueChanged,
            this, [this](int speed) {
                m_weatherRadarSpeedValue->setText(
                    QStringLiteral("%1×").arg(speed / 100.0, 0, 'f', 2));
                writePskSetting("weatherRadarSpeedPercent", speed);
                m_mapView->setWeatherRadarPlaybackSpeed(speed);
            });
    connect(m_globeCheck, &QCheckBox::toggled, this, [this](bool on) {
        writePskSetting("showGlobe", on);
        m_mapView->setProjectionMode(on
            ? MapDisplayWidget::ProjectionMode::Globe
            : MapDisplayWidget::ProjectionMode::Flat);
    });
    connect(m_allCallsignsCheck, &QCheckBox::toggled, this, [this](bool on) {
        writePskSetting("showAllCallsigns", on);
        m_activeMonitorsCheck->setEnabled(on);
        if (m_started) {
            // The global client is prefetched for the entire dialog lifetime,
            // so this toggle is a cache-only display operation.
            if (!m_globalClient->isRunning()) {
                restartGlobalClient();
            }
            // start() may restore the cached snapshot and emit spotsUpdated(),
            // which arms the normal 250 ms burst-coalescing timer. We are
            // rendering that same snapshot synchronously below, so leaving
            // the timer armed only cancels the first expensive marker image
            // and starts it over just as it is about to appear.
            m_markerRefreshTimer->stop();
            rebuildMarkers();
        }
    });
    connect(m_activeMonitorsCheck, &QCheckBox::toggled, this, [this](bool on) {
        writePskSetting("showActiveMonitors", on);
        m_markerRefreshTimer->stop();
        rebuildMarkers();
    });
    connect(m_terminatorCheck, &QCheckBox::toggled, this, [this](bool on) {
        writePskSetting("showTerminator", on);
        m_mapView->setDayNightTerminatorVisible(on);
    });
    connect(m_queryCallsign, &QLineEdit::editingFinished,
            this, &PskReporterMapDialog::applyMapCallsign);
    connect(m_queryCallsign, &QLineEdit::returnPressed,
            this, &PskReporterMapDialog::applyMapCallsign);
    connect(m_queryCallsign, &QLineEdit::textEdited, this, [this] {
        m_mapCallsignUserEdited = true;
    });
    connect(m_mapView, &MapDisplayWidget::markerClicked, this,
            [](const MapView::Marker& marker) {
                if (!marker.clickInfo.isEmpty()) {
                    QToolTip::showText(QCursor::pos(), marker.clickInfo);
                }
            });
    connect(m_mapView,
            &MapDisplayWidget::weatherRadarTimelineLoadingChanged,
            this, [this](bool loading) {
                m_weatherRadarTimelineLoading = loading;
                if (loading) {
                    m_weatherRadarPlayButton->setText(
                        QStringLiteral("■"));
                    m_weatherRadarPlayButton->setAccessibleName(
                        tr("Cancel loading historical weather radar"));
                    m_weatherRadarFrameLabel->setText(tr("Loading"));
                } else {
                    m_weatherRadarPlayButton->setText(
                        m_mapView->weatherRadarAnimating()
                            ? QStringLiteral("❚❚")
                            : QStringLiteral("▶"));
                    m_weatherRadarPlayButton->setAccessibleName(
                        m_mapView->weatherRadarAnimating()
                            ? tr("Pause historical weather radar")
                            : tr("Play historical weather radar"));
                }
            });
    connect(m_mapView,
            &MapDisplayWidget::weatherRadarAnimationStateChanged,
            this, [this](bool playing) {
                m_weatherRadarPlayButton->setText(
                    playing ? QStringLiteral("❚❚")
                            : QStringLiteral("▶"));
                m_weatherRadarPlayButton->setAccessibleName(
                    playing ? tr("Pause historical weather radar")
                            : tr("Play historical weather radar"));
            });
    connect(m_mapView, &MapDisplayWidget::weatherRadarFrameChanged,
            this, [this](const QDateTime& frameTime, bool live) {
                const WeatherRadarFramePresentation presentation =
                    weatherRadarFramePresentation(frameTime, live);
                m_weatherRadarFrameLabel->setText(presentation.text);
                m_weatherRadarFrameLabel->setToolTip(presentation.tooltip + (live ? QString{} : tr(
                    "\nRegional playback uses original observations at or before this clock, up to 10 minutes earlier; missing regions stay transparent.")));
            });
    connect(m_mapView, &MapDisplayWidget::weatherRadarAnimationError,
            this, [this](const QString& message) {
                m_weatherRadarTimelineLoading = false;
                m_weatherRadarPlayButton->setText(QStringLiteral("▶"));
                m_weatherRadarFrameLabel->setText(tr("Unavailable"));
                m_weatherRadarFrameLabel->setToolTip(message);
                QToolTip::showText(
                    m_weatherRadarPlayButton->mapToGlobal(
                        QPoint(0, m_weatherRadarPlayButton->height())),
                    message, m_weatherRadarPlayButton);
            });

    // Keep one footer within the map pane. Band conditions belong to the
    // sidebar; attribution remains on the map in both projections.
    auto* statusBar = new QFrame(mapPanel);
    statusBar->setObjectName(QStringLiteral("pskReporterStatusBar"));
    statusBar->setAccessibleName(tr("PSK Reporter status"));
    ThemeManager::instance().applyStyleSheet(statusBar, QStringLiteral(
        "QFrame#pskReporterStatusBar { background: {{color.background.1}};"
        " border-top: 1px solid {{color.border.subtle}}; }"));
    auto* footerLayout = new QVBoxLayout(statusBar);
    footerLayout->setContentsMargins(6, 4, 6, 4);
    footerLayout->setSpacing(3);
    auto* bottomBar = new QHBoxLayout();
    bottomBar->setSpacing(6);
    footerLayout->addLayout(bottomBar);
    auto* legendLabel = new QLabel(statusBar);
    legendLabel->setObjectName(QStringLiteral("pskReporterModeLegend"));
    legendLabel->setAccessibleName(tr("Report mode colours"));
    legendLabel->setWordWrap(true);
    QStringList legendEntries;
    for (const char* mode : { "FT8", "FT4", "WSPR", "JS8", "CW", "PSK",
                              "RTTY", "SSB", "Other" }) {
        const QString name = QString::fromLatin1(mode);
        legendEntries.append(QStringLiteral(
            "<span style='white-space:nowrap'><span style='color:%1'>●</span>&nbsp;%2</span>")
            .arg(modeColor(name).name(), name));
    }
    legendLabel->setText(legendEntries.join(QStringLiteral("  ")));
    ThemeManager::instance().applyStyleSheet(legendLabel, QStringLiteral(
        "QLabel { color: {{color.text.secondary}}; background: transparent; font-size: 10px; }"));
    bottomBar->addWidget(legendLabel, 1);

    if (m_propForecast != nullptr) {
        auto* conditionsBox = new QGroupBox(tr("Band conditions"), controls);
        QFormLayout* conditionsForm = formFor(conditionsBox);
        for (int i = 0; i < 4; ++i) {
            auto* pill = new QLabel(conditionsBox);
            pill->setAlignment(Qt::AlignCenter);
            m_bandCondPills[i] = pill;
            conditionsForm->addRow(pill);
        }
        // Immediately after Reports, before the map/overlay configuration.
        sections->insertWidget(2, conditionsBox);
        connect(m_propForecast, &PropForecastClient::detailUpdated, this,
                [this] { updateBandConditions(); });
    }
    m_statusLabel = new QLabel(bodyWidget());
    m_statusLabel->setObjectName(QStringLiteral("pskReporterUpdateStatus"));
    m_statusLabel->setAccessibleName(tr("PSK Reporter update status"));
    ThemeManager::instance().applyStyleSheet(m_statusLabel, QStringLiteral(
        "QLabel { color: {{color.text.secondary}}; background: transparent; }"));
    m_statusLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_statusLabel->setWordWrap(true);
    footerLayout->addWidget(m_dxLabel);
    footerLayout->addWidget(m_statusLabel);
    // Connection indicator pinned to the bottom-right corner: "MQTT"/"HTTP"
    // plus a status bullet (green=connected w/ data, yellow=no data,
    // red=no good connection).
    m_connLabel = new QLabel(bodyWidget());
    m_connLabel->setObjectName(QStringLiteral("pskReporterConnection"));
    ThemeManager::instance().applyStyleSheet(m_connLabel, QStringLiteral(
        "QLabel { background: transparent; }"));
    m_connLabel->setAccessibleName(tr("PSK Reporter connection status"));
    m_connLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bottomBar->addSpacing(10);
    bottomBar->addWidget(m_connLabel);
    mapLayout->addWidget(statusBar);

    connect(m_client, &PskReporterClient::connectionStateChanged,
            this, &PskReporterMapDialog::updateConnectionIndicator);
    connect(m_globalClient, &PskReporterClient::connectionStateChanged,
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
        const bool visibleMonitorsEmpty = !m_activeMonitorsCheck->isChecked()
                                       || m_globalClient->monitors().isEmpty();
        const bool globalEmpty = !m_allCallsignsCheck->isChecked()
            || (m_globalClient->spots().isEmpty() && visibleMonitorsEmpty);
        if (globalEmpty && m_client->spots().isEmpty()) {
            if (!m_appliedMapCallsign.isEmpty()) {
                m_statusLabel->setText(
                    tr("No live reports yet for %1 — reports typically appear "
                       "within 1–2 minutes after transmitting.")
                        .arg(m_appliedMapCallsign));
            } else if (m_allCallsignsCheck->isChecked()) {
                m_statusLabel->setText(
                    tr("No reports or active monitors were returned for the "
                       "selected timeframe."));
            } else {
                m_statusLabel->setText(
                    tr("Enter a callsign or enable All Callsigns."));
            }
        }
    });

    m_activeMonitorsCheck->setEnabled(m_allCallsignsCheck->isChecked());

    const int savedLookback = pskSettings().value("lookbackSec").toInt(60 * 60);
    const int lbIdx = m_lookbackCombo->findData(savedLookback);
    m_lookbackCombo->setCurrentIndex(lbIdx >= 0 ? lbIdx : 2);  // default 1h
    m_client->setLookbackSeconds(m_lookbackCombo->currentData().toInt());
    m_globalClient->setLookbackSeconds(
        m_lookbackCombo->currentData().toInt());

    // Debounce rapid Lookback changes into a single deep HTTP query, so
    // spinning through options doesn't hammer PSK Reporter (which 503s).
    m_lookbackDebounce = new QTimer(this);
    m_lookbackDebounce->setSingleShot(true);
    m_lookbackDebounce->setInterval(750);
    connect(m_lookbackDebounce, &QTimer::timeout, this, [this] {
        m_client->setLookbackSeconds(m_lookbackCombo->currentData().toInt());
        m_globalClient->setLookbackSeconds(
            m_lookbackCombo->currentData().toInt());
    });

    connect(m_lookbackCombo, &QComboBox::currentIndexChanged,
            this, &PskReporterMapDialog::onLookbackChanged);
    connect(m_bandCombo, &QComboBox::currentIndexChanged,
            this, [this] { rebuildMarkers(); });
    connect(m_modeCombo, &QComboBox::currentIndexChanged,
            this, [this] { rebuildMarkers(); });
    // MQTT can deliver several reports in one short burst. Repainting the
    // complete map for each message wastes work and can keep invalidating a
    // render that is already in flight. The first report opens a short batch
    // window; later reports join it rather than postponing it indefinitely.
    m_markerRefreshTimer = new QTimer(this);
    m_markerRefreshTimer->setSingleShot(true);
    m_markerRefreshTimer->setInterval(250);
    connect(m_markerRefreshTimer, &QTimer::timeout,
            this, &PskReporterMapDialog::rebuildMarkers);
    connect(m_client, &PskReporterClient::spotsUpdated, this, [this] {
        if (!m_markerRefreshTimer->isActive()) {
            m_markerRefreshTimer->start();
        }
    });
    connect(m_globalClient, &PskReporterClient::spotsUpdated, this, [this] {
        if (!m_markerRefreshTimer->isActive()) {
            m_markerRefreshTimer->start();
        }
    });
    connect(m_client, &PskReporterClient::statusChanged,
            this, [this](const QString& status) {
                m_statusLabel->setText(
                    tr("Live %1: %2").arg(m_appliedMapCallsign, status));
            });
    connect(m_globalClient, &PskReporterClient::statusChanged,
            this, [this](const QString& status) {
                m_statusLabel->setText(tr("All Callsigns: %1").arg(status));
            });
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
            setBeaconStatus(
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
    // The WSPR field is station-owned. It remains intentionally separate from
    // the map-only Call filter above: editing this field updates the persisted
    // station identity, while editing or clearing the map filter never does.
    connect(m_beaconCallsign, &QLineEdit::editingFinished, this, [this] {
        const QString call = m_beaconCallsign->text().trimmed().toUpper();
        m_beaconCallsign->setText(call);
        // Empty is "no change", never "erase". This preserves the existing
        // guard against an accidental select-all/delete wiping station state.
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
        setBeaconStatus(
            tr("%1 · %2 MHz USB on transmit")
                .arg(m_beaconBand->currentText())
                .arg(dialMhz, 0, 'f', 6), "color.text.secondary");
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
        connect(m_radioModel, &RadioModel::callsignChanged, this,
                [this] {
                    // Follow a late-arriving station identity only until the
                    // operator explicitly edits the map-local filter.
                    if (!m_mapCallsignUserEdited) {
                        const QString stationCall =
                            m_radioModel->callsign().trimmed().toUpper().left(32);
                        if (stationCall != m_queryCallsign->text()) {
                            m_queryCallsign->setText(stationCall);
                            if (m_started) {
                                m_appliedMapCallsign = stationCall;
                                restartCallsignClient();
                            }
                        }
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
    const QString stationCall =
        m_radioModel->callsign().trimmed().toUpper();
    // A radio status update must not erase or replace an operator's
    // in-progress edit. An empty station identity is likewise not an erase
    // command for this TX field.
    if (!stationCall.isEmpty() && !m_beaconCallsign->isModified()) {
        m_beaconCallsign->setText(stationCall);
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
        stopBeacon(tr("Cancelled"), BeaconStopOutcome::Cancelled);
        return;
    }
    if (m_audioEngine == nullptr || m_radioModel == nullptr) {
        setBeaconStatus(tr("TX audio is unavailable"));
        return;
    }
    if (m_audioEngine->isRadeMode()) {
        setBeaconStatus(tr("Stop RADE first"));
        return;
    }

    // Ask the connected radio family whether it can key at all before any of
    // the Flex-shaped preconditions below. An RX-only backend (RFC §6,
    // capabilities().canTransmit == false) has no transmitter to check.
    if (!m_radioModel->backendCapabilities().canTransmit) {
        setBeaconStatus(tr("This radio cannot transmit"));
        return;
    }
    TransmitModel& tx = m_radioModel->transmitModel();
    if (tx.isTransmitting() || tx.isTuning()) {
        setBeaconStatus(tr("Transmitter is already in use"));
        return;
    }
    SliceModel* slice = m_radioModel->txSlice();
    if (slice == nullptr) {
        setBeaconStatus(tr("No TX slice is selected"));
        return;
    }
    if (slice->isLocked()) {
        setBeaconStatus(tr("TX slice is locked"));
        return;
    }
    const int timeoutMs = tx.interlockTimeout();
    if (!WsprBeacon::isInterlockTimeoutSufficient(timeoutMs)) {
        setBeaconStatus(
            tr("Radio TX timeout is %1 s; set Radio Setup → TX → Timeout to at least 120 s")
                .arg(timeoutMs / 1000));
        return;
    }

    const WsprBeacon::EncodeResult encoded = WsprBeacon::encode(
        m_beaconCallsign->text(), m_beaconGrid->text(),
        m_beaconPower->currentData().toInt());
    if (!encoded) {
        setBeaconStatus(encoded.error);
        return;
    }

    // Reassert the visible band/mode/filter selection in case another client
    // changed the TX slice after the operator selected the WSPR band.
    if (!applyBeaconBand()) {
        setBeaconStatus(tr("WSPR TX audio route is unavailable"));
        return;
    }
    if (!m_radioModel->prepareWsprTransmit()) {
        restoreBorrowedTxState();  // applyBeaconBand() already borrowed it
        setBeaconStatus(tr("WSPR TX audio route is unavailable"));
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

void PskReporterMapDialog::setBeaconStatus(const QString& text, const char* colourToken)
{
    m_beaconStatus->setText(text);
    m_beaconStatus->setToolTip(text);
    const QString token = QString::fromLatin1(colourToken);
    if (m_beaconStatusDot->property("statusColourToken").toString() != token) {
        m_beaconStatusDot->setProperty("statusColourToken", token);
        ThemeManager::instance().applyStyleSheet(m_beaconStatusDot,
            QStringLiteral("QLabel { background: {{%1}}; border: none; border-radius: 4px; }")
                .arg(token));
    }
}

void PskReporterMapDialog::stopBeacon(const QString& status, BeaconStopOutcome outcome)
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
    switch (outcome) {
    case BeaconStopOutcome::Completed:
        setBeaconStatus(status, "color.accent.success");
        break;
    case BeaconStopOutcome::Cancelled:
        setBeaconStatus(status, "color.text.secondary");
        break;
    case BeaconStopOutcome::Interrupted:
        setBeaconStatus(status, "color.accent.warning");
        break;
    }
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
            stopBeacon(tr("Complete"), BeaconStopOutcome::Completed);
            return;
        }
        const int symbol = std::max(0, beacon->currentSymbol());
        setBeaconStatus(
            tr("Transmitting · symbol %1/%2")
                .arg(std::min(symbol + 1, WsprBeacon::kSymbolCount))
                .arg(WsprBeacon::kSymbolCount), "color.highlight.tx");
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 remainingMs = std::max<qint64>(0, m_beaconSlotMs - nowMs);
    if (remainingMs > 0) {
        setBeaconStatus(
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
    setBeaconStatus(tr("Transmitting · pre-roll"), "color.highlight.tx");
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

void PskReporterMapDialog::onLookbackChanged(int index)
{
    // Persist immediately; defer the (networked) client update so rapid
    // toggling coalesces into one query.
    writePskSetting("lookbackSec", m_lookbackCombo->itemData(index).toInt());
    m_lookbackDebounce->start();
}

void PskReporterMapDialog::restartClients()
{
    m_appliedMapCallsign = m_queryCallsign->text().trimmed().toUpper();
    // Claim the one shared HTTP retrieval slot for the reusable global
    // snapshot as soon as the map opens. Its cached rows also seed the
    // prepopulated callsign while MQTT supplies new reports in parallel.
    restartGlobalClient();
    restartCallsignClient();
    m_emptyStateTimer->start();
}

void PskReporterMapDialog::restartCallsignClient()
{
    m_client->stop();
    m_client->setCallsign(m_appliedMapCallsign);
    m_client->setLookbackSeconds(m_lookbackCombo->currentData().toInt());
    if (!m_appliedMapCallsign.isEmpty()) {
        m_client->start(PskReporterClient::kLiveMqtt);
    }
}

void PskReporterMapDialog::restartGlobalClient()
{
    m_globalClient->stop();
    m_globalClient->setCallsign(QString());
    m_globalClient->setLookbackSeconds(
        m_lookbackCombo->currentData().toInt());
    // Keep this client warm even while its layer is hidden. One global query
    // can seed the callsign history and makes a later All Callsigns toggle
    // immediate; subsequent refreshes retain PSK Reporter's five-minute floor.
    m_globalClient->start(PskReporterClient::kMinPollMs);
    updateConnectionIndicator();
}

void PskReporterMapDialog::applyMapCallsign()
{
    const QString normalized =
        m_queryCallsign->text().trimmed().toUpper().left(32);
    m_queryCallsign->setText(normalized);
    if (m_started && normalized != m_appliedMapCallsign) {
        m_appliedMapCallsign = normalized;
        restartCallsignClient();
        rebuildMarkers();
        m_emptyStateTimer->start();
    }
}

void PskReporterMapDialog::updateConnectionIndicator()
{
    const bool globalEnabled = m_allCallsignsCheck->isChecked();
    const bool globalRunning = m_globalClient->isRunning();
    const bool callsignEnabled = !m_appliedMapCallsign.isEmpty();
    const bool up = (globalRunning && m_globalClient->lastHttpOk())
                 || (callsignEnabled && m_client->isMqttConnected());
    const bool hasData = (globalRunning
                          && (!m_globalClient->spots().isEmpty()
                              || !m_globalClient->monitors().isEmpty()))
                      || (callsignEnabled && !m_client->spots().isEmpty());
    const bool sawError = (globalRunning && m_globalClient->sawError())
                       || (callsignEnabled && m_client->sawError());
    QString color;
    if (!up) {
        color = sawError ? QStringLiteral("#e74c3c")
                         : QStringLiteral("#f4c20d");
    } else {
        color = hasData ? QStringLiteral("#2ecc71")
                        : QStringLiteral("#f4c20d");
    }

    QStringList transports;
    if (globalRunning) {
        transports.append(QStringLiteral("HTTP"));
    }
    if (callsignEnabled) {
        transports.append(QStringLiteral("MQTT"));
    }
    if (transports.isEmpty()) {
        transports.append(tr("Off"));
    }
    m_connLabel->setText(
        QStringLiteral("%1 <span style='color:%2;'>&#9679;</span>")
            .arg(transports.join(QStringLiteral(" + ")), color));

    QStringList diagnostics;
    diagnostics.append(globalRunning
        ? tr("Global HTTP cache%1: %2 report(s), %3 active monitor(s)")
              .arg(globalEnabled ? QString() : tr(" (layer hidden)"))
              .arg(m_globalClient->spots().size())
              .arg(m_globalClient->monitors().size())
        : tr("Global HTTP cache: disabled"));
    if (globalRunning) {
        if (m_globalClient->httpRequestInFlight()) {
            diagnostics.append(tr("Global HTTP request: in progress"));
        }
        if (m_globalClient->lastHttpRequestAt().isValid()) {
            diagnostics.append(tr("Last global HTTP request: %1")
                .arg(m_globalClient->lastHttpRequestAt().toLocalTime().toString(
                    QStringLiteral("yyyy-MM-dd hh:mm:ss"))));
        }
        if (m_globalClient->lastHttpStatus() > 0) {
            diagnostics.append(tr("Last global HTTP status: %1")
                                   .arg(m_globalClient->lastHttpStatus()));
        }
        if (!m_globalClient->lastHttpError().isEmpty()) {
            diagnostics.append(tr("Global HTTP error: %1")
                                   .arg(m_globalClient->lastHttpError()));
        }
        const QDateTime nextRequest = m_globalClient->nextHttpRequestAt();
        if (nextRequest.isValid()
            && nextRequest > QDateTime::currentDateTime()) {
            diagnostics.append(tr("Next global HTTP request allowed: %1")
                .arg(nextRequest.toLocalTime().toString(
                    QStringLiteral("yyyy-MM-dd hh:mm:ss"))));
        }
    }
    diagnostics.append(callsignEnabled
        ? tr("Live %1 MQTT: %2 (%3 cached report(s))")
              .arg(m_appliedMapCallsign,
                   m_client->isMqttConnected() ? tr("connected")
                                               : tr("connecting"))
              .arg(m_client->spots().size())
        : tr("Live callsign MQTT: disabled"));
    const QString tooltip = diagnostics.join(QLatin1Char('\n'));
    m_connLabel->setToolTip(tooltip);
    m_connLabel->setAccessibleDescription(tooltip);
}

void PskReporterMapDialog::rebuildMarkers()
{
    struct DisplaySpot {
        const PskReporterSpot* spot{nullptr};
        bool callsignSpecific{false};
    };

    QVector<DisplaySpot> displaySpots;
    QHash<QString, int> spotIndexes;
    const auto mergeSpot = [&displaySpots, &spotIndexes](
                               const PskReporterSpot& spot,
                               bool callsignSpecific) {
        const QString key = spot.senderCallsign.toUpper()
                          + QLatin1Char('|')
                          + spot.receiverCallsign.toUpper()
                          + QLatin1Char('|')
                          + QString::number(spot.frequencyHz)
                          + QLatin1Char('|') + spot.mode.toUpper();
        const auto existing = spotIndexes.constFind(key);
        if (existing == spotIndexes.cend()) {
            spotIndexes.insert(key, displaySpots.size());
            displaySpots.append({&spot, callsignSpecific});
            return;
        }
        DisplaySpot& current = displaySpots[*existing];
        if (spot.flowStartSeconds >= current.spot->flowStartSeconds) {
            current.spot = &spot;
        }
        // Preserve live-layer identity even when the five-minute global
        // snapshot has the newer copy of this same report.
        current.callsignSpecific = current.callsignSpecific
                                 || callsignSpecific;
    };
    const auto mergeSpots = [&mergeSpot](
                                const QVector<PskReporterSpot>& spots,
                                bool callsignSpecific) {
        for (const PskReporterSpot& spot : spots) {
            mergeSpot(spot, callsignSpecific);
        }
    };

    const bool globalEnabled = m_allCallsignsCheck->isChecked();
    if (globalEnabled) {
        mergeSpots(m_globalClient->spots(), false);
    } else if (!m_appliedMapCallsign.isEmpty()) {
        // MQTT has no history. Reuse matching rows from the prefetched global
        // snapshot as callsign-specific data without spending a second HTTP
        // request slot.
        for (const PskReporterSpot& spot : m_globalClient->spots()) {
            if (spot.senderCallsign.compare(
                    m_appliedMapCallsign, Qt::CaseInsensitive) == 0
                || spot.receiverCallsign.compare(
                       m_appliedMapCallsign, Qt::CaseInsensitive) == 0) {
                mergeSpot(spot, true);
            }
        }
    }
    if (!m_appliedMapCallsign.isEmpty()) {
        // Merge the live layer second so an equal-timestamp report is rendered
        // with callsign-specific labeling and hover behavior.
        mergeSpots(m_client->spots(), true);
    }

    QVector<MapView::Marker> markers;
    constexpr int kMaxRenderedMonitors = 3000;

    // GPS and the saved beacon grid are the preferred station-location
    // sources. If neither exists, PSK Reporter itself can still tell us the
    // locator our station advertised in one of its reports. Only use the real
    // station callsign here: entering some other call in the map field must
    // not move "home" to that station.
    if (!m_mapView->hasHomePosition() && m_radioModel != nullptr) {
        const QString stationCall = m_radioModel->callsign().trimmed();
        if (!stationCall.isEmpty()) {
            for (const DisplaySpot& displaySpot : displaySpots) {
                const PskReporterSpot& spot = *displaySpot.spot;
                QString locator;
                if (spot.senderCallsign.compare(
                        stationCall, Qt::CaseInsensitive) == 0) {
                    locator = spot.senderLocator;
                } else if (spot.receiverCallsign.compare(
                               stationCall, Qt::CaseInsensitive) == 0) {
                    locator = spot.receiverLocator;
                }
                double stationLat = 0.0;
                double stationLon = 0.0;
                if (MaidenheadLocator::toLatLon(
                        locator, stationLat, stationLon)) {
                    m_mapView->setHomePosition(
                        stationLat, stationLon, stationCall);
                    break;
                }
            }
        }
    }
    markers.reserve(displaySpots.size()
                    + (globalEnabled
                           ? qMin(m_globalClient->monitors().size(),
                                  kMaxRenderedMonitors)
                           : 0));
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
    MapView::Marker selectedCallsignMarker;
    qint64 selectedCallsignMarkerTime = -1;
    bool hasSelectedCallsignMarker = false;

    int renderedMonitors = 0;
    if (globalEnabled && m_activeMonitorsCheck->isChecked()) {
        for (const PskReporterMonitor& monitor : m_globalClient->monitors()) {
            if (renderedMonitors >= kMaxRenderedMonitors) {
                break;
            }
            if (!bandFilter.isEmpty()
                && (monitor.frequencyHz <= 0
                    || bandName(monitor.frequencyHz) != bandFilter)) {
                continue;
            }
            if (!modeFilter.isEmpty()
                && (monitor.mode.isEmpty()
                    || modeGroup(monitor.mode) != modeFilter)) {
                continue;
            }
            double lat = 0.0;
            double lon = 0.0;
            if (!MaidenheadLocator::toLatLon(monitor.locator, lat, lon)) {
                continue;
            }
            MapView::Marker marker;
            marker.lat = lat;
            marker.lon = lon;
            marker.isMonitor = true;
            marker.pathEnabled = false;
            marker.color = modeColor(monitor.mode);
            const QString software = monitor.decoderSoftware.isEmpty()
                ? QString() : QStringLiteral("<br>%1")
                                      .arg(monitor.decoderSoftware.toHtmlEscaped());
            marker.tooltip = QStringLiteral(
                "<div style='white-space:nowrap;'><b>%1</b> %2"
                "<br>Active monitor · %3%4</div>")
                .arg(monitor.callsign.toHtmlEscaped(),
                     monitor.locator.toHtmlEscaped(),
                     monitor.mode.isEmpty() ? tr("mode not reported")
                                            : monitor.mode.toHtmlEscaped(),
                     software);
            marker.clickInfo = marker.tooltip;
            markers.append(marker);
            ++renderedMonitors;
        }
    }

    int renderedLiveReports = 0;
    for (const DisplaySpot& displaySpot : displaySpots) {
        const PskReporterSpot& spot = *displaySpot.spot;
        const QString queryCall = m_appliedMapCallsign;
        const bool selectedCallReport = !queryCall.isEmpty()
            && (spot.senderCallsign.compare(queryCall, Qt::CaseInsensitive) == 0
                || spot.receiverCallsign.compare(
                       queryCall, Qt::CaseInsensitive) == 0);
        // A matching global report belongs to the selected callsign's hover
        // group too. This matters immediately after enabling All Callsigns,
        // before MQTT has repeated every report in the HTTP snapshot.
        const bool callsignSpecific = displaySpot.callsignSpecific
                                   || selectedCallReport;
        if (spot.flowStartSeconds < cutoff) {
            continue;
        }
        if (!bandFilter.isEmpty() && bandName(spot.frequencyHz) != bandFilter) {
            continue;
        }
        if (!modeFilter.isEmpty() && modeGroup(spot.mode) != modeFilter) {
            continue;
        }
        const bool queryIsReceiver = callsignSpecific
            && spot.receiverCallsign.compare(queryCall, Qt::CaseInsensitive) == 0;
        const QString queryLocator = queryIsReceiver ? spot.receiverLocator
                                                     : spot.senderLocator;
        const QString markerCall = queryIsReceiver ? spot.senderCallsign
                                                   : spot.receiverCallsign;
        const QString markerLocator = queryIsReceiver ? spot.senderLocator
                                                      : spot.receiverLocator;
        const QString originLocator = queryIsReceiver ? spot.receiverLocator
                                                      : spot.senderLocator;
        double queryLat = 0.0;
        double queryLon = 0.0;
        if (callsignSpecific
            && spot.flowStartSeconds >= selectedCallsignMarkerTime
            && MaidenheadLocator::toLatLon(queryLocator, queryLat, queryLon)) {
            selectedCallsignMarker.lat = queryLat;
            selectedCallsignMarker.lon = queryLon;
            selectedCallsignMarker.label = queryCall;
            selectedCallsignMarker.color = modeColor(spot.mode);
            selectedCallsignMarker.pathEnabled = false;
            selectedCallsignMarker.pathGroup = queryCall.toUpper();
            selectedCallsignMarker.hoverShowsPathGroup = true;
            selectedCallsignMarker.tooltip = buildSpotCard(
                spot, false, 0.0, 0.0, queryLat, queryLon);
            selectedCallsignMarker.clickInfo = selectedCallsignMarker.tooltip;
            selectedCallsignMarkerTime = spot.flowStartSeconds;
            hasSelectedCallsignMarker = true;
        }
        double lat = 0.0;
        double lon = 0.0;
        if (!MaidenheadLocator::toLatLon(markerLocator, lat, lon)) {
            continue;
        }
        MapView::Marker m;
        m.lat = lat;
        m.lon = lon;
        m.label = callsignSpecific ? markerCall : QString();
        m.color = modeColor(spot.mode);
        m.pathGroup = callsignSpecific ? queryCall.toUpper() : QString();
        double originLat = 0.0;
        double originLon = 0.0;
        if (MaidenheadLocator::toLatLon(originLocator, originLat, originLon)) {
            m.hasPathOrigin = true;
            m.pathFromLat = originLat;
            m.pathFromLon = originLon;
        }
        const bool queryIsStation = m_radioModel != nullptr
            && queryCall.compare(m_radioModel->callsign(), Qt::CaseInsensitive) == 0;
        m.pathEnabled = m.hasPathOrigin
                     || (callsignSpecific && queryIsStation);
        // Same compact card for hover and click.
        const QString card = buildSpotCard(
            spot, m.hasPathOrigin || hasHome,
            m.hasPathOrigin ? originLat : m_mapView->homeLat(),
            m.hasPathOrigin ? originLon : m_mapView->homeLon(),
            lat, lon);
        m.tooltip = card;
        m.clickInfo = card;
        markers.append(m);
        if (displaySpot.callsignSpecific) {
            ++renderedLiveReports;
        }

        bandsHeard.insert(bandName(spot.frequencyHz));
        if (m.hasPathOrigin || hasHome) {
            const double km = MaidenheadLocator::distanceKm(
                m.hasPathOrigin ? originLat : m_mapView->homeLat(),
                m.hasPathOrigin ? originLon : m_mapView->homeLon(), lat, lon);
            if (km > bestKm) {
                bestKm = km;
                farthestCall = markerCall;
            }
        }
    }
    const bool queryIsHomeStation = m_radioModel != nullptr
        && m_appliedMapCallsign.compare(
               m_radioModel->callsign(), Qt::CaseInsensitive) == 0;
    if (hasSelectedCallsignMarker && !(queryIsHomeStation && hasHome)) {
        // A callsign-specific report renders its opposite endpoint above. Keep
        // one marker for the queried station too, using its newest valid
        // advertised locator. The real station's existing home marker remains
        // authoritative when available, so it is never duplicated or moved by
        // an arbitrary map query.
        markers.append(selectedCallsignMarker);
    }
    m_mapView->setMarkers(markers);

    // Reception stats (top-right): count · bands · farthest.
    QStringList parts;
    const int selectedMarkerCount = hasSelectedCallsignMarker
                                 && !(queryIsHomeStation && hasHome) ? 1 : 0;
    const int reportMarkers = markers.size() - renderedMonitors
                            - selectedMarkerCount;
    if (globalEnabled) {
        parts << tr("%n report(s)", nullptr, reportMarkers);
        if (m_activeMonitorsCheck->isChecked()) {
            parts << tr("%n active monitor(s)", nullptr, renderedMonitors);
        }
        if (renderedLiveReports > 0) {
            parts << tr("%1 live: %2")
                         .arg(m_appliedMapCallsign)
                         .arg(renderedLiveReports);
        }
        if (m_globalClient->resultsLimited()
            || m_globalClient->monitors().size() > kMaxRenderedMonitors) {
            parts << tr("display capped");
        }
    } else if (!markers.isEmpty()) {
        parts << tr("%n spot(s)", nullptr, reportMarkers);
        parts << tr("%n band(s)", nullptr, bandsHeard.size());
        if (bestKm >= 0.0) {
            parts << tr("farthest %1 %L2 km").arg(farthestCall).arg(qRound(bestKm));
        }
    }
    m_dxLabel->setText(parts.join(QStringLiteral("  •  ")));
    m_dxLabel->setVisible(!parts.isEmpty());

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
    m_mapView->setWeatherRadarVisible(m_weatherRadarCheck->isChecked());
    m_mapView->setRadarCoverageVisible(m_radarCoverageCheck->isChecked());
    m_mapView->setWeatherRadarPlaybackSpeed(
        m_weatherRadarSpeedSlider->value());
    updateHomeFromRadio();
    if (m_propForecast != nullptr) {
        // Refresh the detailed forecast (band conditions) on open; the
        // client guards against overlapping in-flight requests.
        m_propForecast->fetchDetail();
        updateBandConditions();
    }
    if (!m_started) {
        m_started = true;
        restartClients();
    }
}

void PskReporterMapDialog::closeEvent(QCloseEvent* event)
{
    if (m_beaconArmed || m_beaconTransmitting) {
        stopBeacon(tr("Stopped"), BeaconStopOutcome::Cancelled);
    }
    // Stop hitting the network while the window is closed.
    m_client->stop();
    m_globalClient->stop();
    m_mapView->setWeatherRadarVisible(false);
    m_mapView->setRadarCoverageVisible(false);
    m_started = false;
    PersistentDialog::closeEvent(event);
}

} // namespace AetherSDR
