#include "PropForecastClient.h"
#include "AppSettings.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>
#include <cmath>

Q_LOGGING_CATEGORY(lcPropForecast, "mastersdr.propforecast")

namespace MasterSDR {

PropForecastClient::PropForecastClient(QObject* parent)
    : QObject(parent)
{
    m_timer.setSingleShot(false);
    m_timer.setInterval(kIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &PropForecastClient::onTimer);
}

// ── Enable / disable ─────────────────────────────────────────────────────────

void PropForecastClient::setEnabled(bool on)
{
    if (m_enabled == on) { return; }
    m_enabled = on;

    if (on) {
        fetch();
        m_timer.start();
    } else {
        m_timer.stop();
    }
}

// ── Timer callback ───────────────────────────────────────────────────────────

void PropForecastClient::onTimer()
{
    fetch();
}

// ── Current conditions fetch (wwv.txt) ───────────────────────────────────────

void PropForecastClient::fetch()
{
    if (m_fetchInFlight) {
        qCDebug(lcPropForecast) << "fetch already in flight — skipping";
        return;
    }
    m_fetchInFlight = true;

    QNetworkRequest req{QUrl{QString(kUrl)}};
    req.setHeader(QNetworkRequest::UserAgentHeader, "MasterSDR");
    auto* reply = m_nam.get(req);

    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_fetchInFlight = false;

        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(lcPropForecast) << "fetch failed:" << reply->errorString();
            emit fetchError(reply->errorString());
            return;
        }

        QString text = QString::fromUtf8(reply->readAll());
        PropForecast fc;

        // K-index: fractional value from WWV (#1232, #1255, #1401)
        static const QRegularExpression reK(
            QStringLiteral("K-index.*\\bwas\\s+(\\d+\\.?\\d*)"));
        auto mK = reK.match(text);
        if (mK.hasMatch())
            fc.kIndex = mK.captured(1).toDouble();

        // A-index
        static const QRegularExpression reA(
            QStringLiteral("A-index\\s+(\\d+)"));
        auto mA = reA.match(text);
        if (mA.hasMatch())
            fc.aIndex = mA.captured(1).toInt();

        // Solar flux
        static const QRegularExpression reSFI(
            QStringLiteral("Solar flux\\s+(\\d+)"));
        auto mSFI = reSFI.match(text);
        if (mSFI.hasMatch())
            fc.sfi = mSFI.captured(1).toInt();

        if (fc.kIndex >= 0 && fc.aIndex >= 0 && fc.sfi > 0) {
            m_last = fc;
            emit forecastUpdated(m_last);
            qCDebug(lcPropForecast) << "updated — K" << fc.kIndex << "A" << fc.aIndex
                                    << "SFI" << fc.sfi;
        } else {
            qCWarning(lcPropForecast) << "failed to parse WWV bulletin — kIndex:" << fc.kIndex
                                      << "aIndex:" << fc.aIndex << "sfi:" << fc.sfi;
        }
    });
}

// ── Detail fetch (3-day forecast + daily indices) ────────────────────────────

void PropForecastClient::fetchDetail()
{
    m_detail = PropForecastDetail{};
    m_detailPending = 4;  // forecast + indices + band conditions + xray

    auto makeReq = [](const char* url) {
        QNetworkRequest req{QUrl{QString(url)}};
        req.setHeader(QNetworkRequest::UserAgentHeader, "MasterSDR");
        return req;
    };

    auto checkDone = [this]() {
        if (--m_detailPending <= 0 && m_detail.valid)
            emit detailUpdated(m_detail);
    };

    // Fetch 3-day forecast
    auto* r1 = m_nam.get(makeReq(kForecastUrl));
    connect(r1, &QNetworkReply::finished, this, [this, r1, checkDone] {
        r1->deleteLater();
        if (r1->error() == QNetworkReply::NoError)
            parseForecast(QString::fromUtf8(r1->readAll()));
        checkDone();
    });

    // Fetch daily solar indices
    auto* r2 = m_nam.get(makeReq(kIndicesUrl));
    connect(r2, &QNetworkReply::finished, this, [this, r2, checkDone] {
        r2->deleteLater();
        if (r2->error() == QNetworkReply::NoError)
            parseDailyIndices(QString::fromUtf8(r2->readAll()));
        checkDone();
    });

    // Fetch band conditions from N0NBH
    auto* r3 = m_nam.get(makeReq(kBandUrl));
    connect(r3, &QNetworkReply::finished, this, [this, r3, checkDone] {
        r3->deleteLater();
        if (r3->error() == QNetworkReply::NoError)
            parseBandConditions(QString::fromUtf8(r3->readAll()));
        checkDone();
    });

    // Fetch X-ray flux
    auto* r4 = m_nam.get(makeReq(kXrayUrl));
    connect(r4, &QNetworkReply::finished, this, [this, r4, checkDone] {
        r4->deleteLater();
        if (r4->error() == QNetworkReply::NoError)
            parseXrayFlux(r4->readAll());
        checkDone();
    });
}

void PropForecastClient::parseForecast(const QString& text)
{
    // Parse 3-day Kp forecast table from 3-day-forecast.txt
    // Format:
    //              Apr 14       Apr 15       Apr 16
    // 00-03UT       2.00         2.67         1.67
    // ...

    // Extract day labels from the header line
    static const QRegularExpression reDayHeader(
        QStringLiteral("^\\s+(\\w+\\s+\\d+)\\s+(\\w+\\s+\\d+)\\s+(\\w+\\s+\\d+)"),
        QRegularExpression::MultilineOption);
    auto mDays = reDayHeader.match(text);
    if (mDays.hasMatch()) {
        m_detail.dayLabels[0] = mDays.captured(1).trimmed();
        m_detail.dayLabels[1] = mDays.captured(2).trimmed();
        m_detail.dayLabels[2] = mDays.captured(3).trimmed();
    }

    // Parse Kp rows: "HH-HHUT  val  val  val"
    static const QRegularExpression reKpRow(
        QStringLiteral("^(\\d{2}-\\d{2}UT)\\s+(\\d+\\.\\d+)\\s+(\\d+\\.\\d+)\\s+(\\d+\\.\\d+)"),
        QRegularExpression::MultilineOption);
    auto it = reKpRow.globalMatch(text);
    int row = 0;
    while (it.hasNext() && row < 8) {
        auto m = it.next();
        m_detail.kpForecast[0][row] = m.captured(2).toDouble();
        m_detail.kpForecast[1][row] = m.captured(3).toDouble();
        m_detail.kpForecast[2][row] = m.captured(4).toDouble();
        ++row;
    }

    // Compute max Kp per day
    for (int d = 0; d < 3; ++d) {
        double mx = 0;
        for (int p = 0; p < 8; ++p)
            mx = std::max(mx, m_detail.kpForecast[d][p]);
        m_detail.maxKp[d] = mx;
    }

    // Parse radio blackout forecast
    // R1-R2           10%           10%           10%
    // R3 or greater    1%            1%            1%
    static const QRegularExpression reR1R2(
        QStringLiteral("R1-R2\\s+(\\d+)%\\s+(\\d+)%\\s+(\\d+)%"),
        QRegularExpression::MultilineOption);
    auto mR1 = reR1R2.match(text);
    if (mR1.hasMatch()) {
        m_detail.blackoutR1R2[0] = mR1.captured(1).toInt();
        m_detail.blackoutR1R2[1] = mR1.captured(2).toInt();
        m_detail.blackoutR1R2[2] = mR1.captured(3).toInt();
    }

    static const QRegularExpression reR3(
        QStringLiteral("R3 or greater\\s+(\\d+)%\\s+(\\d+)%\\s+(\\d+)%"),
        QRegularExpression::MultilineOption);
    auto mR3 = reR3.match(text);
    if (mR3.hasMatch()) {
        m_detail.blackoutR3[0] = mR3.captured(1).toInt();
        m_detail.blackoutR3[1] = mR3.captured(2).toInt();
        m_detail.blackoutR3[2] = mR3.captured(3).toInt();
    }

    // Parse solar radiation forecast
    // S1 or greater    1%      1%      1%
    static const QRegularExpression reS1(
        QStringLiteral("S1 or greater\\s+(\\d+)%\\s+(\\d+)%\\s+(\\d+)%"),
        QRegularExpression::MultilineOption);
    auto mS1 = reS1.match(text);
    if (mS1.hasMatch()) {
        m_detail.radiationS1[0] = mS1.captured(1).toInt();
        m_detail.radiationS1[1] = mS1.captured(2).toInt();
        m_detail.radiationS1[2] = mS1.captured(3).toInt();
    }

    // Extract rationale texts
    static const QRegularExpression reGeoRat(
        QStringLiteral("Rationale: (No G1.*?(?:\\.|$))"),
        QRegularExpression::MultilineOption | QRegularExpression::DotMatchesEverythingOption);
    auto mGR = reGeoRat.match(text);
    if (mGR.hasMatch())
        m_detail.geomagRationale = mGR.captured(1).simplified();

    // Blackout rationale is the last "Rationale:" in the text
    static const QRegularExpression reBlkRat(
        QStringLiteral("Rationale: (There is.*?(?:\\.|$))"),
        QRegularExpression::MultilineOption | QRegularExpression::DotMatchesEverythingOption);
    auto mBR = reBlkRat.match(text);
    if (mBR.hasMatch())
        m_detail.blackoutRationale = mBR.captured(1).simplified();

    if (row > 0)
        m_detail.valid = true;

    qCDebug(lcPropForecast) << "parsed 3-day forecast:" << row << "Kp rows,"
                            << "days:" << m_detail.dayLabels[0] << m_detail.dayLabels[1] << m_detail.dayLabels[2];
}

void PropForecastClient::parseDailyIndices(const QString& text)
{
    // Parse the last non-empty data line from daily-solar-indices.txt
    // Format: YYYY MM DD  flux  sunspot  area  regions  field  bkgd  C M X S 1 2 3
    const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
    for (int i = lines.size() - 1; i >= 0; --i) {
        const QString& line = lines[i];
        if (line.startsWith('#') || line.startsWith(':'))
            continue;

        const QStringList fields = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (fields.size() >= 5) {
            // fields[3] = flux, fields[4] = sunspot number
            m_detail.solarFlux10cm = fields[3].toInt();
            m_detail.sunspotNumber = fields[4].toInt();
            qCDebug(lcPropForecast) << "parsed daily indices — SFI:" << m_detail.solarFlux10cm
                                    << "SSN:" << m_detail.sunspotNumber;
            break;
        }
    }
}

void PropForecastClient::parseBandConditions(const QString& xml)
{
    // Parse N0NBH solarxml.php — simple XML with <band> and <phenomenon> elements.
    // Using regex since we don't need a full XML parser for this.

    // Band conditions: <band name="80m-40m" time="day">Good</band>
    static const QRegularExpression reBand(
        QStringLiteral("<band name=\"([^\"]+)\" time=\"([^\"]+)\">([^<]+)</band>"));
    static const char* bandNames[] = {"80m-40m", "30m-20m", "17m-15m", "12m-10m"};

    auto it = reBand.globalMatch(xml);
    while (it.hasNext()) {
        auto m = it.next();
        QString name = m.captured(1);
        QString time = m.captured(2);
        QString cond = m.captured(3).trimmed();

        for (int i = 0; i < 4; ++i) {
            if (name == bandNames[i]) {
                if (time == "day")
                    m_detail.bandDay[i] = cond;
                else
                    m_detail.bandNight[i] = cond;
                break;
            }
        }
    }

    // VHF conditions: <phenomenon name="..." location="...">Band Closed</phenomenon>
    static const QRegularExpression reVhf(
        QStringLiteral("<phenomenon name=\"([^\"]+)\" location=\"([^\"]+)\">([^<]+)</phenomenon>"));
    auto vit = reVhf.globalMatch(xml);
    while (vit.hasNext()) {
        auto m = vit.next();
        QString name = m.captured(1);
        QString loc = m.captured(2);
        QString cond = m.captured(3).trimmed();

        if (name == "vhf-aurora")
            m_detail.vhfAurora = cond;
        else if (name == "E-Skip" && loc == "north_america")
            m_detail.esNorthAmerica = cond;
        else if (name == "E-Skip" && loc == "europe")
            m_detail.esEurope = cond;
    }

    // Extra solar data from N0NBH
    static const QRegularExpression reField(
        QStringLiteral("<geomagfield>([^<]+)</geomagfield>"));
    auto mf = reField.match(xml);
    if (mf.hasMatch())
        m_detail.geomagField = mf.captured(1).trimmed();

    static const QRegularExpression reNoise(
        QStringLiteral("<signalnoise>([^<]+)</signalnoise>"));
    auto mn = reNoise.match(xml);
    if (mn.hasMatch())
        m_detail.signalNoise = mn.captured(1).trimmed();

    static const QRegularExpression reWind(
        QStringLiteral("<solarwind>([^<]+)</solarwind>"));
    auto mw = reWind.match(xml);
    if (mw.hasMatch())
        m_detail.solarWind = mw.captured(1).trimmed().toDouble();

    static const QRegularExpression reXray(
        QStringLiteral("<xray>([^<]+)</xray>"));
    auto mx = reXray.match(xml);
    if (mx.hasMatch())
        m_detail.xrayClass = mx.captured(1).trimmed();

    qCDebug(lcPropForecast) << "parsed N0NBH — bands:"
                            << m_detail.bandDay[0] << "/" << m_detail.bandNight[0]
                            << "geomag:" << m_detail.geomagField
                            << "xray:" << m_detail.xrayClass;
}

void PropForecastClient::parseXrayFlux(const QByteArray& json)
{
    // Parse GOES X-ray flux JSON — array of objects with energy "0.1-0.8nm"
    // We want the latest reading for the long-wavelength channel.
    QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isArray()) return;

    const QJsonArray arr = doc.array();
    for (int i = arr.size() - 1; i >= 0; --i) {
        QJsonObject obj = arr[i].toObject();
        if (obj["energy"].toString() == "0.1-0.8nm") {
            double flux = obj["flux"].toDouble();
            if (flux > 0) {
                // Convert to flare class: A=1e-8, B=1e-7, C=1e-6, M=1e-5, X=1e-4
                int exp = static_cast<int>(std::floor(std::log10(flux)));
                double mantissa = flux / std::pow(10.0, exp);
                static const char classes[] = {'?','?','?','?','X','M','C','B','A'};
                int idx = -exp;
                char cls = (idx >= 0 && idx < 9) ? classes[idx] : '?';
                m_detail.xrayClass = QString("%1%2").arg(cls).arg(mantissa, 0, 'f', 1);
                qCDebug(lcPropForecast) << "parsed GOES X-ray:" << m_detail.xrayClass
                                        << "flux:" << flux;
            }
            break;
        }
    }
}

} // namespace MasterSDR
