#include "OperaRadarNetwork.h"
#include "OperaRadarImage.h"
#include "WeatherRadarSource.h"
#include "MapProviderNetworkAccessManager.h"
#include "RegionalRadarComposite.h"
#include "LibreRadarNetwork.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QXmlStreamReader>
#include <QNetworkDiskCache>
#include <QStandardPaths>
#include <QBuffer>
#include <QCache>
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QTimeZone>
#include <algorithm>
#include <utility>
#include <QUrlQuery>
#include <QtConcurrent/QtConcurrentRun>
#include <cmath>
#include <cstring>

namespace AetherSDR {
namespace {
class ImageReply final : public QNetworkReply {
public:
    explicit ImageReply(const QNetworkRequest& request, QObject* parent) : QNetworkReply(parent)
    {
        setRequest(request); setUrl(request.url()); setOperation(QNetworkAccessManager::GetOperation);
        open(QIODevice::ReadOnly); setFinished(false);
    }
    void abort() override { finish({}, QStringLiteral("Radar request canceled"), OperationCanceledError); }
    qint64 bytesAvailable() const override { return m_bytes.size() - m_offset + QNetworkReply::bytesAvailable(); }
    bool isSequential() const override { return true; }
    void finish(const QByteArray& bytes, const QString& error = {}, NetworkError code = UnknownContentError)
    {
        if (isFinished()) { return; }
        m_bytes = bytes;
        if (!error.isEmpty()) { setError(code, error); setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 503); }
        else { setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200); setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("image/png")); }
        setFinished(true);
        if (!error.isEmpty()) { emit errorOccurred(code); }
        if (!bytes.isEmpty()) { emit readyRead(); }
        emit finished();
    }
protected:
    qint64 readData(char* data, qint64 size) override
    {
        const qint64 count = std::min(size, qint64(m_bytes.size()) - m_offset);
        if (count <= 0) { return isFinished() ? -1 : 0; }
        std::memcpy(data, m_bytes.constData() + m_offset, size_t(count)); m_offset += count; return count;
    }
private:
    QByteArray m_bytes;
    qint64 m_offset{0};
};

class OperaRenderer final : public QObject {
public:
    explicit OperaRenderer(QObject* parent) : QObject(parent), m_network(this), m_images(70 * 1024), m_raw(128 * 1024)
    {
        auto* cache = new QNetworkDiskCache(&m_network);
        cache->setCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/opera-radar"));
        cache->setMaximumCacheSize(512 * 1024 * 1024);
        m_network.setCache(cache);
    }
    QNetworkReply* request(const QNetworkRequest& request, QObject* parent)
    {
        auto* reply = new ImageReply(request, parent);
        QTimer::singleShot(0, this, [this, reply = QPointer<ImageReply>(reply)] {
            if (!reply || reply->isFinished()) { return; }
            if (reply->url().host() == QStringLiteral("timeline")) {
                catalog([reply](const QByteArray& bytes) {
                    if (reply) { reply->finish(bytes, bytes.isEmpty() ? QStringLiteral("European catalog unavailable") : QString{}); }
                });
                return;
            }
            prune();
            if (m_pending.size() >= 256) { reply->finish({}, QStringLiteral("European radar queue full")); return; }
            m_pending.append(reply); pump();
        });
        return reply;
    }
private:
    void prune()
    {
        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(), [](const auto& r) { return !r || r->isFinished(); }), m_pending.end());
    }
    void fail(const QString& error)
    {
        const auto pending = std::exchange(m_pending, {});
        for (const auto& reply : pending) { if (reply) { reply->finish({}, error); } }
        m_busy = false;
        m_retryAt = QDateTime::currentDateTimeUtc().addSecs(66);
    }
    void pump()
    {
        prune();
        if (m_busy || m_pending.isEmpty()) { return; }
        if (m_retryAt > QDateTime::currentDateTimeUtc()) { const auto retryAt = m_retryAt; fail(QStringLiteral("European radar temporarily unavailable; retrying after cooldown")); m_retryAt = retryAt; return; }
        const QUrlQuery query(m_pending.first()->url());
        const QString time = query.queryItemValue(QStringLiteral("time"));
        if (time == QStringLiteral("latest") && (!m_latest.isValid()
            || m_catalogAt.secsTo(QDateTime::currentDateTimeUtc()) >= 300)) {
            m_busy = true;
            catalog([this](const QByteArray& bytes) {
                const auto frames = WeatherRadarSource(WeatherRadarSource::Provider::Opera).parseTimeline(bytes, 1);
                if (frames.isEmpty()) { fail(QStringLiteral("European radar timeline unavailable")); return; }
                m_latest = frames.last().frameTime; m_busy = false; pump();
            });
            return;
        }
        bool ok = false;
        const QDateTime frame = time == QStringLiteral("latest") ? m_latest
            : QDateTime::fromSecsSinceEpoch(time.toLongLong(&ok), QTimeZone::UTC);
        if ((!ok && time != QStringLiteral("latest")) || !frame.isValid()
            || frame < QDateTime::currentDateTimeUtc().addSecs(-5 * 3600)
            || frame > QDateTime::currentDateTimeUtc().addSecs(600)) {
            m_pending.takeFirst()->finish({}, QStringLiteral("Invalid European radar frame")); pump(); return;
        }
        const qint64 key = frame.toSecsSinceEpoch();
        if (OperaRadarImage* raster = m_images.object(key)) {
            deliver(key, *raster); pump(); return;
        }
        m_busy = true;
        if (QByteArray* raw = m_raw.object(key)) { decode(key, *raw); return; }
        fetch(QNetworkRequest(WeatherRadarSource::operaFrameUrl(frame)), 16 * 1024 * 1024,
            [this, key](const QByteArray& bytes) {
                m_raw.insert(key, new QByteArray(bytes), int(bytes.size() / 1024 + 1));
                decode(key, bytes);
            });
    }
    void fetch(QNetworkRequest request, qint64 limit, std::function<void(const QByteArray&)> success,
        std::function<void()> failure = {})
    {
        request.setTransferTimeout(30000);
        if (request.url().path().endsWith(QStringLiteral(".tiff"))
            || request.url().path().endsWith(QStringLiteral(".tif"))) {
            request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,QNetworkRequest::PreferCache);
        }
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("AetherSDR (https://github.com/aethersdr/AetherSDR)"));
        QNetworkReply* reply = m_network.get(request);
        connect(reply, &QNetworkReply::downloadProgress, reply, [reply, limit](qint64 n, qint64) { if (n > limit) { reply->abort(); } });
        connect(reply, &QNetworkReply::finished, this, [this, reply, limit, success, failure] {
            const QByteArray bytes = reply->error() == QNetworkReply::NoError ? reply->read(limit + 1) : QByteArray{};
            const QString error = reply->errorString(); reply->deleteLater();
            if (bytes.isEmpty() || bytes.size() > limit) {
                if (failure) { failure(); } else { fail(QStringLiteral("European radar download failed: ") + error); }
                return;
            }
            success(bytes);
        });
    }
    void catalog(std::function<void(const QByteArray&)> callback)
    {
        if (!m_catalog.isEmpty() && m_catalogAt.secsTo(QDateTime::currentDateTimeUtc()) < 300) {
            callback(m_catalog); return;
        }
        m_catalogWaiters.append(std::move(callback));
        if (m_catalogWaiters.size() > 1) { return; }
        m_catalogLinks = {}; m_catalogFailed = false;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const QDateTime first = now.addSecs(-4 * 3600 - 600);
        m_catalogParts = first.date() == now.date() ? 1 : 2;
        for (QDate day = first.date(); day <= now.date(); day = day.addDays(1)) {
            const QString prefix = day.toString(QStringLiteral("yyyy/MM/dd")) + QStringLiteral("/OPERA/COMP/");
            QUrl url(QStringLiteral("https://s3.waw3-1.cloudferro.com/openradar-24h"));
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("list-type"), QStringLiteral("2"));
            query.addQueryItem(QStringLiteral("prefix"), prefix);
            query.addQueryItem(QStringLiteral("max-keys"), QStringLiteral("1000"));
            // Six product/format variants per timestamp. Bound each listing to
            // the requested hours; no pagination or full-bucket scan needed.
            query.addQueryItem(QStringLiteral("start-after"), prefix + QStringLiteral("OPERA@")
                + (day == first.date() ? first.toString(QStringLiteral("yyyyMMdd'T'HHmm")) : day.toString(QStringLiteral("yyyyMMdd")) + QStringLiteral("T0000")));
            url.setQuery(query);
            fetch(QNetworkRequest(url), 512 * 1024, [this](const QByteArray& bytes) {
                QXmlStreamReader xml(bytes);
                while (!xml.atEnd()) {
                    xml.readNext();
                    if (xml.isStartElement() && xml.name() == QStringLiteral("Key")) {
                        const QString key = xml.readElementText();
                        if (key.endsWith(QStringLiteral("@0@DBZH.tiff"))) {
                            m_catalogLinks.append(QJsonObject{{QStringLiteral("href"),
                                QStringLiteral("https://s3.waw3-1.cloudferro.com/openradar-24h/") + key}});
                        }
                    } else if (xml.isStartElement() && xml.name() == QStringLiteral("IsTruncated")) {
                        if (xml.readElementText() != QStringLiteral("false")) { m_catalogFailed = true; }
                    }
                }
                m_catalogFailed = m_catalogFailed || xml.hasError();
                catalogPartFinished();
            }, [this] { m_catalogFailed = true; catalogPartFinished(); });
        }
    }
    void catalogPartFinished()
    {
        if (--m_catalogParts != 0) { return; }
        const QByteArray bytes = m_catalogFailed ? QByteArray{} : QJsonDocument(
            QJsonObject{{QStringLiteral("links"), m_catalogLinks}}).toJson(QJsonDocument::Compact);
        if (!bytes.isEmpty()) {
            m_catalog = bytes; m_catalogAt = QDateTime::currentDateTimeUtc();
            const auto frames = WeatherRadarSource(WeatherRadarSource::Provider::Opera).parseTimeline(bytes, 1);
            if (!frames.isEmpty()) { m_latest = frames.last().frameTime; }
        }
        const auto callbacks = std::exchange(m_catalogWaiters, {});
        for (const auto& callback : callbacks) { callback(bytes); }
    }
    void decode(qint64 key, const QByteArray& bytes)
    {
        auto* watcher = new QFutureWatcher<OperaRadarImage>(this);
        connect(watcher, &QFutureWatcher<OperaRadarImage>::finished, this, [this, watcher, key] {
            const OperaRadarImage result = watcher->result(); watcher->deleteLater();
            if (result.image.isNull()) { fail(result.error); return; }
            m_images.insert(key, new OperaRadarImage(result), int(result.image.sizeInBytes() / 1024 + 1));
            m_busy = false; deliver(key, result); pump();
        });
        watcher->setFuture(QtConcurrent::run([bytes] { return OperaRadarImage::decode(bytes); }));
    }
    void deliver(qint64 key, const OperaRadarImage& raster)
    {
        prune();
        const auto pending = m_pending;
        for (const auto& reply : pending) {
            if (!reply || reply->isFinished()) { continue; }
            const QUrlQuery q(reply->url());
            const QString t = q.queryItemValue(QStringLiteral("time"));
            if ((t == QStringLiteral("latest") ? m_latest.toSecsSinceEpoch() : t.toLongLong()) != key) { continue; }
            const QStringList b = q.queryItemValue(QStringLiteral("bbox")).split(',');
            bool valid = b.size() == 4;
            double v[4]{};
            for (int i = 0; valid && i < 4; ++i) { bool ok; v[i] = b[i].toDouble(&ok); valid = ok && std::isfinite(v[i]) && std::abs(v[i]) <= 40075017; }
            const int w = q.queryItemValue(QStringLiteral("width")).toInt(), h = q.queryItemValue(QStringLiteral("height")).toInt();
            if (!valid || v[2] <= v[0] || v[3] <= v[1] || w <= 0 || h <= 0 || w > 4096 || h > 4096) {
                reply->finish({}, QStringLiteral("Invalid radar export bounds")); continue;
            }
            QImage image(w, h, QImage::Format_RGBA8888_Premultiplied); image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            const QRectF target((raster.bounds.left() - v[0]) / (v[2] - v[0]) * w,
                (v[3] - raster.bounds.bottom()) / (v[3] - v[1]) * h,
                raster.bounds.width() / (v[2] - v[0]) * w, raster.bounds.height() / (v[3] - v[1]) * h);
            painter.drawImage(target, raster.image); painter.end();
            QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG");
            reply->finish(png);
        }
        prune();
    }
    MapProviderNetworkAccessManager m_network;
    QCache<qint64, OperaRadarImage> m_images;
    QCache<qint64, QByteArray> m_raw;
    QVector<QPointer<ImageReply>> m_pending;
    QDateTime m_latest, m_catalogAt, m_retryAt;
    QByteArray m_catalog;
    QJsonArray m_catalogLinks;
    QVector<std::function<void(const QByteArray&)>> m_catalogWaiters;
    int m_catalogParts{0};
    bool m_catalogFailed{false};
    bool m_busy{false};
};
}

void installOperaRadarNetwork()
{
    static QPointer<OperaRenderer> renderer;
    if (!renderer) { renderer = new OperaRenderer(QCoreApplication::instance()); }
    MapProviderNetworkAccessManager::radarRequestHandler = [](const QNetworkRequest& request, QObject* parent) -> QNetworkReply* {
        if (request.url().scheme() == QStringLiteral("radar-composite")) { return requestRegionalRadar(request, parent); }
        if (request.url().scheme() == QStringLiteral("libre-radar")) { return requestLibreRadar(request, parent); }
        return renderer ? renderer->request(request, parent) : nullptr;
    };
}
}
