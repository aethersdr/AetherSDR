#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QString>
#include <array>

namespace AetherSDR {

struct PropForecast {
    double kIndex{-1.0};  // Planetary K-index 0-9; -1 = not yet fetched
    int aIndex{-1};       // Planetary A-index; -1 = not yet fetched
    int sfi{-1};          // Solar Flux Index (SFU); -1 = not yet fetched
};

// Detailed propagation forecast for the dashboard dialog.
struct PropForecastDetail {
    // 3-day Kp forecast: [day][period], 8 periods per day (00-03UT .. 21-00UT)
    double kpForecast[3][8]{};
    double maxKp[3]{};                // max Kp per day
    QString dayLabels[3];             // e.g. "Apr 14", "Apr 15", "Apr 16"

    // Radio blackout probabilities (%)
    int blackoutR1R2[3]{};            // R1-R2 Minor-Moderate
    int blackoutR3[3]{};              // R3+ Major

    // Solar radiation probabilities (%)
    int radiationS1[3]{};             // S1+

    // Daily solar indices (latest day)
    int sunspotNumber{-1};
    int solarFlux10cm{-1};            // 10.7cm radio flux

    // Rationale text
    QString geomagRationale;
    QString blackoutRationale;

    // Band conditions from N0NBH (day/night × 4 band groups)
    // Indices: 0=80m-40m, 1=30m-20m, 2=17m-15m, 3=12m-10m
    QString bandDay[4];       // "Good", "Fair", "Poor"
    QString bandNight[4];
    QString geomagField;      // "QUIET", "UNSETTLED", "ACTIVE", "STORM"
    QString signalNoise;      // "S0-S1", "S2-S3", etc.
    double solarWind{-1.0};   // km/s
    QString xrayClass;        // "B4.9", "C1.2", "M2.3", etc.

    // VHF conditions
    QString vhfAurora;        // "Band Closed", "Band Open"
    QString esNorthAmerica;
    QString esEurope;

    // Lunar phase from NASA Dial-A-Moon
    QString lunarPhaseName;   // "Waning Crescent", "Full Moon", etc.
    int lunarIllumination{-1}; // % illuminated

    bool valid{false};
};

// Fetches HF propagation conditions from NOAA SWPC public data.
// Primary: wwv.txt (current K/A/SFI) once per hour.
// Detail: 3-day-forecast.txt + daily-solar-indices.txt on demand.
//
// Hardening:
//   - Guards against overlapping in-flight requests.
class PropForecastClient : public QObject {
    Q_OBJECT

public:
    explicit PropForecastClient(QObject* parent = nullptr);

    void setEnabled(bool on);
    bool isEnabled() const { return m_enabled; }

    PropForecast lastForecast() const { return m_last; }
    PropForecastDetail lastDetail() const { return m_detail; }

    // Fetch detailed forecast (3-day + daily indices) for the dashboard.
    // Emits detailUpdated() when complete.
    void fetchDetail();

signals:
    void forecastUpdated(const PropForecast& forecast);
    void detailUpdated(const PropForecastDetail& detail);
    void fetchError(const QString& error);

private slots:
    void onTimer();

private:
    void fetch();
    void parseForecast(const QString& text);
    void parseDailyIndices(const QString& text);
    void parseBandConditions(const QString& xml);
    void parseXrayFlux(const QByteArray& json);

    QNetworkAccessManager m_nam;
    QTimer               m_timer;
    PropForecast         m_last;
    PropForecastDetail   m_detail;
    bool                 m_enabled{false};
    bool                 m_fetchInFlight{false};
    int                  m_detailPending{0};  // count of outstanding detail fetches

    static constexpr const char* kUrl         = "https://services.swpc.noaa.gov/text/wwv.txt";
    static constexpr const char* kForecastUrl = "https://services.swpc.noaa.gov/text/3-day-forecast.txt";
    static constexpr const char* kIndicesUrl  = "https://services.swpc.noaa.gov/text/daily-solar-indices.txt";
    static constexpr const char* kBandUrl     = "https://www.hamqsl.com/solarxml.php";
    static constexpr const char* kXrayUrl     = "https://services.swpc.noaa.gov/json/goes/primary/xrays-6-hour.json";
    static constexpr int kIntervalMs  = 60 * 60 * 1000;  // 1 hour
};

} // namespace AetherSDR
