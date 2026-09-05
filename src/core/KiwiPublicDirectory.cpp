#include "KiwiPublicDirectory.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <memory>

namespace AetherSDR {

namespace {
// Per-request transfer timeout so a stalled server can't leave the picker stuck
// on "Loading…" forever (the only escape would otherwise be Cancel).
constexpr int kDirectoryFetchTimeoutMs = 15000;

// Longest sysop-supplied string we keep.  The body is already capped, but a
// single pathological field should not reach the UI at megabyte length either
// (Principle VII — bound it where the bytes enter).
constexpr int kMaxFieldChars = 512;

QString boundedString(const QJsonValue& v)
{
    if (!v.isString())
        return QString();
    QString s = v.toString().trimmed();
    if (s.size() > kMaxFieldChars)
        s.truncate(kMaxFieldChars);
    return s;
}

// The origin publishes offline as a real JSON bool; older snapshots used the
// string "yes"/"no".  Anything else is "not offline".
bool readOffline(const QJsonValue& v)
{
    if (v.isBool())
        return v.toBool();
    if (v.isString()) {
        const QString s = v.toString().trimmed();
        return s.compare(QLatin1String("yes"), Qt::CaseInsensitive) == 0
            || s.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
    }
    return false;
}

// A non-negative count, or 0 for anything missing or nonsensical.
int readCount(const QJsonValue& v)
{
    if (!v.isDouble())
        return 0;
    const double d = v.toDouble(0.0);
    if (!std::isfinite(d) || d <= 0.0)
        return 0;
    return static_cast<int>(std::min(d, 1.0e6));
}

// A finite number from a JSON array slot, or fallback.
double readNumber(const QJsonArray& a, int i, double fallback)
{
    if (i >= a.size() || !a.at(i).isDouble())
        return fallback;
    const double d = a.at(i).toDouble(fallback);
    return std::isfinite(d) ? d : fallback;
}
} // namespace

QByteArray KiwiPublicDirectory::userAgent()
{
    // Honest identity — we are AetherSDR, not a browser.  If an operator
    // chooses to block this, that is their answer and we honor it.  The version
    // comes from the build (AETHERSDR_VERSION) so the identity can't go stale.
#ifdef AETHERSDR_VERSION
    return QByteArrayLiteral("AetherSDR/" AETHERSDR_VERSION
                             " (+https://github.com/aethersdr/AetherSDR)");
#else
    return QByteArrayLiteral("AetherSDR (+https://github.com/aethersdr/AetherSDR)");
#endif
}

QString KiwiPublicReceiver::apiBadge() const
{
    switch (apiPolicy()) {
        case ApiPolicy::Disabled: return QStringLiteral("Web only (API disabled by operator)");
        case ApiPolicy::Limited:  return QStringLiteral("API: %1 of %2 channels").arg(extApi).arg(usersMax);
        case ApiPolicy::Open:     return QStringLiteral("API: %1 channels").arg(extApi);
        case ApiPolicy::Unknown:  break;
    }
    return QStringLiteral("API policy unknown");
}

bool KiwiPublicReceiver::advertisesConnectionLimit() const
{
    // Match "Limits" as a whole whitespace-delimited token, not a substring, so
    // a free-form hardware/firmware descriptor that merely contains those letters
    // (e.g. "Unlimited", "NoLimitsBeta") can't false-positive. The public
    // directory appends the marker as its own token in sdr_hw.
    const QStringList tokens =
        sdrHw.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        if (token.compare(QStringLiteral("Limits"), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

QString KiwiPublicReceiver::connectionLimitBadge() const
{
    return advertisesConnectionLimit()
        ? QStringLiteral("Limits")
        : QString();
}

QString KiwiPublicReceiver::bandRangeLabel() const
{
    if (!hasBands)
        return QString();
    const auto mhz = [](qint64 hz) {
        return QString::number(static_cast<double>(hz) / 1e6, 'f', 1);
    };
    return QStringLiteral("%1 – %2 MHz").arg(mhz(bandLowHz), mhz(bandHighHz));
}

KiwiDirectoryParse KiwiPublicDirectory::parse(const QByteArray& json)
{
    KiwiDirectoryParse result;

    if (json.isEmpty()) {
        result.error = QStringLiteral("empty response from the receiver directory");
        return result;
    }
    if (json.size() > kMaxBodyBytes) {
        result.error = QStringLiteral("receiver directory is implausibly large (%1 bytes)")
                           .arg(json.size());
        return result;
    }

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        result.error = QStringLiteral("receiver directory is not valid JSON (%1)")
                           .arg(perr.error != QJsonParseError::NoError
                                    ? perr.errorString()
                                    : QStringLiteral("expected a JSON object"));
        return result;
    }
    const QJsonObject root = doc.object();

    // Schema gate.  We refuse to parse on hopefully: the fields this class
    // exists to get right (ext_api above all) could have changed meaning.
    const QJsonValue schemaVal = root.value(QStringLiteral("schema"));
    if (!schemaVal.isDouble()) {
        result.error = QStringLiteral("receiver directory has no schema version");
        return result;
    }
    result.schema = schemaVal.toInt(0);
    if (result.schema != kSupportedSchema) {
        result.error = QStringLiteral(
                           "receiver directory uses schema %1, but this build of "
                           "AetherSDR understands only schema %2 — please update "
                           "AetherSDR")
                           .arg(result.schema)
                           .arg(kSupportedSchema);
        return result;
    }

    const QJsonValue rxVal = root.value(QStringLiteral("receivers"));
    if (!rxVal.isArray()) {
        result.error = QStringLiteral("receiver directory has no receiver list");
        return result;
    }
    const QJsonArray arr = rxVal.toArray();
    if (arr.size() > kMaxReceivers) {
        result.error = QStringLiteral("receiver directory lists %1 receivers, "
                                      "far more than any plausible list")
                           .arg(arr.size());
        return result;
    }

    // toUTC() converts; setTimeSpec() would only relabel, discarding the offset
    // of a "+02:00" form and mis-aging the list by exactly that offset.
    result.fetchedAt = QDateTime::fromString(
        boundedString(root.value(QStringLiteral("fetched_at"))), Qt::ISODate);
    if (result.fetchedAt.isValid())
        result.fetchedAt = result.fetchedAt.toUTC();

    QVector<KiwiPublicReceiver> receivers;
    receivers.reserve(arr.size());
    for (const QJsonValue& entry : arr) {
        if (!entry.isObject())
            continue;
        const QJsonObject obj = entry.toObject();

        KiwiPublicReceiver rx;
        rx.url = boundedString(obj.value(QStringLiteral("url")));
        if (rx.url.isEmpty())
            continue;  // an entry with no endpoint is not a receiver

        rx.id       = boundedString(obj.value(QStringLiteral("id")));
        rx.name     = boundedString(obj.value(QStringLiteral("name")));
        rx.location = boundedString(obj.value(QStringLiteral("loc")));
        rx.antenna  = boundedString(obj.value(QStringLiteral("antenna")));
        rx.sdrHw    = boundedString(obj.value(QStringLiteral("sdr_hw")));
        rx.grid     = boundedString(obj.value(QStringLiteral("grid")));

        rx.users    = readCount(obj.value(QStringLiteral("users")));
        rx.usersMax = readCount(obj.value(QStringLiteral("users_max")));

        // THE field this class exists for.  A receiver that does not publish a
        // policy has NO ext_api key — obj["ext_api"].toInt() would hand back 0
        // and silently reclassify every Unknown operator as a Disabled one.
        // That fails safe for the connection decision but misreports the
        // operator's policy in the badge and in the picker's hidden counts,
        // which is exactly what we must not get wrong.
        const QJsonValue extApiVal = obj.value(QStringLiteral("ext_api"));
        rx.extApi = extApiVal.isDouble() ? extApiVal.toInt(-1) : -1;

        rx.offline = readOffline(obj.value(QStringLiteral("offline")));
        rx.flagged = obj.value(QStringLiteral("flagged")).toBool(false);

        const QJsonValue gpsVal = obj.value(QStringLiteral("gps"));
        if (gpsVal.isArray()) {
            const QJsonArray gps = gpsVal.toArray();
            const double lat = readNumber(gps, 0, 0.0);
            const double lon = readNumber(gps, 1, 0.0);
            // A null island fix is the origin's "no position", not a position.
            if (gps.size() >= 2 && std::abs(lat) <= 90.0 && std::abs(lon) <= 180.0
                && !(lat == 0.0 && lon == 0.0)) {
                rx.gpsLat = lat;
                rx.gpsLon = lon;
                rx.hasGps = true;
            }
        }

        const QJsonValue bandsVal = obj.value(QStringLiteral("bands"));
        if (bandsVal.isArray()) {
            const QJsonArray bands = bandsVal.toArray();
            const double low  = readNumber(bands, 0, -1.0);
            const double high = readNumber(bands, 1, -1.0);
            if (bands.size() >= 2 && low >= 0.0 && high > low) {
                rx.bandLowHz  = static_cast<qint64>(low);
                rx.bandHighHz = static_cast<qint64>(high);
                rx.hasBands   = true;
            }
        }

        // snr is [all-band, HF]; the origin also publishes the same pair as
        // scalar snr_all/snr_hf, which we accept as a fallback.
        const QJsonValue snrVal = obj.value(QStringLiteral("snr"));
        if (snrVal.isArray()) {
            const QJsonArray snr = snrVal.toArray();
            rx.snrAll = static_cast<int>(readNumber(snr, 0, -1.0));
            rx.snrHf  = static_cast<int>(readNumber(snr, 1, -1.0));
        }
        if (rx.snrAll < 0 && obj.value(QStringLiteral("snr_all")).isDouble())
            rx.snrAll = obj.value(QStringLiteral("snr_all")).toInt(-1);
        if (rx.snrHf < 0 && obj.value(QStringLiteral("snr_hf")).isDouble())
            rx.snrHf = obj.value(QStringLiteral("snr_hf")).toInt(-1);

        receivers.push_back(rx);
    }

    if (receivers.isEmpty()) {
        result.error = QStringLiteral("the receiver directory is empty");
        return result;
    }

    result.receivers = std::move(receivers);
    return result;
}

KiwiPublicDirectory::KiwiPublicDirectory(QObject* parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
}

void KiwiPublicDirectory::fetch()
{
    // One GET against AetherSDR's mirror.  There is deliberately no fallback
    // to kiwisdr.com: see the good-citizen contract in the header.
    QNetworkRequest req{QUrl(QString::fromLatin1(kDirectoryUrl))};
    req.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    req.setTransferTimeout(kDirectoryFetchTimeoutMs);
    QNetworkReply* reply = m_net->get(req);

    // Bound the transfer where the bytes actually enter (Principle VII):
    // QNetworkReply buffers the whole response, so a cap checked only in
    // parse() would reject an oversized body we had already allocated in full.
    auto oversized = std::make_shared<bool>(false);
    connect(reply, &QNetworkReply::downloadProgress, this,
            [reply, oversized](qint64 received, qint64 total) {
                if (received <= kMaxBodyBytes && total <= kMaxBodyBytes)
                    return;
                *oversized = true;
                reply->abort();
            });

    connect(reply, &QNetworkReply::finished, this, [this, reply, oversized]() {
        reply->deleteLater();
        if (*oversized) {
            emit failed(QStringLiteral("receiver directory is implausibly large "
                                       "(over %1 MB)").arg(kMaxBodyBytes / (1024 * 1024)));
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            emit failed(reply->errorString());
            return;
        }
        const KiwiDirectoryParse parsed = parse(reply->readAll());
        if (!parsed.ok()) {
            emit failed(parsed.error);
            return;
        }
        emit ready(parsed.receivers, parsed.fetchedAt);
    });
}

} // namespace AetherSDR
