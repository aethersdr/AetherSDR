#include "RegionalRadarComposite.h"
#include "MapProviderNetworkAccessManager.h"
#include "WeatherRadarViewGeometry.h"
#include <QBuffer>
#include <QCache>
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkDiskCache>
#include <QStandardPaths>
#include <QPainter>
#include <QPointer>
#include <QTimeZone>
#include <QTimer>
#include <QUrlQuery>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

namespace AetherSDR {
std::optional<WeatherRadarObservation> radarObservationAt(
    const QVector<WeatherRadarObservation>& observations, const QDateTime& time)
{
    std::optional<WeatherRadarObservation> selected;
    for (const auto& observation : observations) {
        if (observation.frameTime <= time && observation.frameTime.secsTo(time) <= 600
            && (!selected || observation.frameTime > selected->frameTime)) {
            selected = observation;
        }
    }
    return selected;
}

QImage composeRegionalRadar(const QVector<QImage>& images, const QSize& size)
{
    if (images.size() > 3 && !images[3].isNull()) { return images[3]; }
    QImage result(size, QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    // Canada, then US, then Europe. NOAA's pixels take precedence in the
    // overlapping North American footprint; units remain source-specific.
    for (int index : {1, 0, 2}) {
        if (index < images.size() && !images[index].isNull()) { painter.drawImage(0, 0, images[index]); }
    }
    return result;
}

namespace {
class CompositeReply final : public QNetworkReply {
public:
    CompositeReply(const QNetworkRequest& request, QObject* parent) : QNetworkReply(parent)
    {
        setRequest(request); setUrl(request.url()); setOperation(QNetworkAccessManager::GetOperation);
        open(QIODevice::ReadOnly); setFinished(false);
    }
    void abort() override { finish({}, QStringLiteral("Canceled"), OperationCanceledError); }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return m_bytes.size() - m_offset + QNetworkReply::bytesAvailable(); }
    void finish(const QByteArray& bytes, const QString& error = {}, NetworkError code = UnknownContentError)
    {
        if (isFinished()) { return; }
        m_bytes = bytes;
        if (!error.isEmpty()) { setError(code, error); }
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, error.isEmpty() ? 200 : 503);
        setHeader(QNetworkRequest::ContentTypeHeader,
            url().host() == QStringLiteral("timeline") ? QStringLiteral("application/json") : QStringLiteral("image/png"));
        setFinished(true);
        if (!error.isEmpty()) { emit errorOccurred(code); }
        if (!bytes.isEmpty()) { emit readyRead(); }
        emit finished();
    }
protected:
    qint64 readData(char* data, qint64 size) override
    {
        const qint64 count = std::min(size, qint64(m_bytes.size()) - m_offset);
        if (count <= 0) { return -1; }
        std::memcpy(data, m_bytes.constData() + m_offset, size_t(count)); m_offset += count; return count;
    }
private:
    QByteArray m_bytes;
    qint64 m_offset{0};
};

struct Export {
    QPointer<CompositeReply> reply;
    QSize size;
    QRectF bounds;
    QVector<QImage> images = QVector<QImage>(4);
    QVector<qint64> noaaRasterIds;
    int remaining{0};
    int succeeded{0};
    bool failed{false};
};

class CompositeRenderer final : public QObject {
public:
    explicit CompositeRenderer(QObject* parent, QNetworkAccessManager* network = nullptr)
        : QObject(parent), m_network(network ? network : new MapProviderNetworkAccessManager(this)), m_cache(64 * 1024)
    {
        if (!network) {
            auto* cache=new QNetworkDiskCache(m_network);
            cache->setCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                +QStringLiteral("/regional-radar"));
            cache->setMaximumCacheSize(256*1024*1024);
            m_network->setCache(cache);
        }
    }
    QNetworkReply* request(const QNetworkRequest& request, QObject* parent)
    {
        auto* reply = new CompositeReply(request, parent);
        QTimer::singleShot(0, this, [this, reply = QPointer<CompositeReply>(reply)] {
            if (!reply || reply->isFinished()) { return; }
            m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(), [](const auto& r) { return !r || r->isFinished(); }), m_queue.end());
            if (m_queue.size() >= 128) { reply->finish({}, QStringLiteral("Radar queue full")); return; }
            m_queue.append(reply); pump();
        });
        return reply;
    }
    QString status(int mask) const
    {
        QStringList failures;
        const QStringList names{QStringLiteral("US"), QStringLiteral("Canada"), QStringLiteral("Europe"), QStringLiteral("LibreWXR")};
        for (int i = 0; i < 4; ++i) {
            if ((mask & (1 << i)) && m_failed[i]) { failures.append(names[i]); }
        }
        if ((mask & 8) && (m_primaryFallback || m_failed[3])) {
            return (mask & 7) ? QStringLiteral("LibreWXR unavailable · using enabled regional backups")
                : QStringLiteral("LibreWXR unavailable");
        }
        return failures.isEmpty() ? QString{} : QStringLiteral("Unavailable: ") + failures.join(QStringLiteral(", "));
    }
private:
    void catalog(int index, std::function<void()> callback)
    {
        if (m_catalogAt[index].isValid() && m_catalogAt[index].secsTo(QDateTime::currentDateTimeUtc()) < (index == 3 ? 300 : 60)) {
            callback(); return;
        }
        m_waiters[index].append(std::move(callback));
        if (m_waiters[index].size() > 1) { return; }
        const WeatherRadarSource source(index == 3 ? WeatherRadarSource::Provider::LibreWxr : static_cast<WeatherRadarSource::Provider>(index));
        QNetworkRequest request(source.timelineUrl());
        request.setTransferTimeout(30000);
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("AetherSDR (https://github.com/aethersdr/AetherSDR)"));
        QNetworkReply* reply = m_network->get(request);
        connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 n, qint64) { if (n > 2 * 1024 * 1024) { reply->abort(); } });
        connect(reply, &QNetworkReply::finished, this, [this, reply, source, index] {
            const auto frames = reply->error() == QNetworkReply::NoError
                ? source.parseTimeline(reply->read(2 * 1024 * 1024 + 1), 4) : QVector<WeatherRadarObservation>{};
            reply->deleteLater();
            m_failed[index] = frames.isEmpty();
            // Retained catalogs cannot turn into current weather: selection has
            // its own ten-minute age bound relative to the requested clock.
            if (!frames.isEmpty()) { m_catalogs[index] = frames; }
            m_catalogAt[index] = QDateTime::currentDateTimeUtc();
            const auto callbacks = std::exchange(m_waiters[index], {});
            for (const auto& callback : callbacks) { callback(); }
        });
    }
    void catalogs(int mask, std::function<void()> callback)
    {
        auto remaining = std::make_shared<int>(0);
        for (int i = 0; i < 4; ++i) { if (mask & (1 << i)) { ++*remaining; } }
        if (*remaining == 0) { callback(); return; }
        for (int i = 0; i < 4; ++i) {
            if (mask & (1 << i)) { catalog(i, [remaining, callback] { if (--*remaining == 0) { callback(); } }); }
        }
    }
    void complete()
    {
        --m_active;
        QTimer::singleShot(0, this, [this] { pump(); });
    }
    void pump()
    {
        while (m_active < 2 && !m_queue.isEmpty()) {
            const QPointer<CompositeReply> reply = m_queue.takeFirst();
            if (!reply || reply->isFinished()) { continue; }
            ++m_active;
            const int mask = QUrlQuery(reply->url()).queryItemValue(QStringLiteral("providers")).toInt();
            if (mask < 0 || mask > 15) { reply->finish({}, QStringLiteral("Invalid radar providers")); complete(); continue; }
            if (reply->url().host() == QStringLiteral("timeline")) {
                auto finish = [this, reply, mask] {
                    if (reply && !reply->isFinished()) { reply->finish(timeline(mask)); }
                    complete();
                };
                if (mask & 8) {
                    catalog(3, [this, mask, finish] {
                        if (m_failed[3]) { catalogs(mask & 7, finish); } else { finish(); }
                    });
                } else { catalogs(mask, finish); }
            } else {
                if (const QByteArray* cached = m_cache.object(reply->url().toString())) {
                    reply->finish(*cached); complete(); continue;
                }
                const bool historical = QUrlQuery(reply->url()).queryItemValue(QStringLiteral("time")) != QStringLiteral("latest");
                if (mask & 8) { catalog(3, [this, reply, mask] { exportImage(reply, mask); }); }
                else if (historical) { catalogs(mask, [this, reply, mask] { exportImage(reply, mask); }); }
                else { exportImage(reply, mask); }
            }
        }
    }
    QByteArray timeline(int mask) const
    {
        if ((mask & 8) && !m_failed[3] && !m_catalogs[3].isEmpty()) {
            QJsonArray times;
            for (const auto& frame : m_catalogs[3]) { times.append(frame.frameTime.toSecsSinceEpoch()); }
            return QJsonDocument(QJsonObject{{QStringLiteral("times"),times},
                {QStringLiteral("providers"),8}}).toJson(QJsonDocument::Compact);
        }
        mask &= 7;
        QDateTime latest;
        for (int i = 0; i < 4; ++i) {
            if ((mask & (1 << i)) && !m_catalogs[i].isEmpty()) { latest = std::max(latest, m_catalogs[i].last().frameTime); }
        }
        QJsonArray times;
        if (latest.isValid()) {
            const qint64 end = latest.toSecsSinceEpoch() / 300 * 300;
            for (qint64 epoch = end - 4 * 3600; epoch <= end; epoch += 300) {
                const QDateTime time = QDateTime::fromSecsSinceEpoch(epoch, QTimeZone::UTC);
                for (int i = 0; i < 4; ++i) {
                    if ((mask & (1 << i)) && radarObservationAt(m_catalogs[i], time)) {
                        times.append(epoch); break;
                    }
                }
            }
        }
        return QJsonDocument(QJsonObject{{QStringLiteral("times"), times},
            {QStringLiteral("providers"),mask}}).toJson(QJsonDocument::Compact);
    }
    void exportImage(QPointer<CompositeReply> reply, int mask)
    {
        if (!reply || reply->isFinished()) { complete(); return; }
        const QUrlQuery query(reply->url());
        const QStringList b = query.queryItemValue(QStringLiteral("bbox")).split(',');
        bool valid = b.size() == 4;
        double v[4]{};
        for (int i = 0; valid && i < 4; ++i) { bool ok; v[i] = b[i].toDouble(&ok); valid = ok && std::isfinite(v[i]) && std::abs(v[i]) <= 40075017; }
        const int w = query.queryItemValue(QStringLiteral("width")).toInt(), h = query.queryItemValue(QStringLiteral("height")).toInt();
        if (!valid || v[2] <= v[0] || v[3] <= v[1] || w <= 0 || h <= 0 || w > 4096 || h > 4096 || qint64(w) * h > kMaximumWeatherRadarPixels) {
            reply->finish({}, QStringLiteral("Invalid radar export bounds")); complete(); return;
        }
        const QString stamp = query.queryItemValue(QStringLiteral("time"));
        bool ok = false;
        const bool live = stamp == QStringLiteral("latest");
        const QDateTime time = QDateTime::fromSecsSinceEpoch(stamp.toLongLong(&ok), QTimeZone::UTC);
        if (!live && (!ok || time < QDateTime::currentDateTimeUtc().addSecs(-5 * 3600) || time > QDateTime::currentDateTimeUtc())) {
            reply->finish({}, QStringLiteral("Invalid radar observation time")); complete(); return;
        }
        auto job = std::make_shared<Export>(); job->reply = reply; job->size = QSize(w,h);
        job->bounds = QRectF(QPointF(v[0],v[1]),QPointF(v[2],v[3]));
        auto backups = [this, job, mask, time, live] {
            if (!job->reply || job->reply->isFinished()) { complete(); return; }
            if (live) { exportBackups(job, mask & 7, time, live); }
            else { catalogs(mask & 7, [this, job, mask, time, live] { exportBackups(job, mask & 7, time, live); }); }
        };
        if (!(mask & 8)) { backups(); return; }
        const QDateTime clock = live ? QDateTime::currentDateTimeUtc() : time;
        const auto observation = live && !m_catalogs[3].isEmpty()
            && m_catalogs[3].last().frameTime.secsTo(clock) <= 1800
            ? std::optional<WeatherRadarObservation>(m_catalogs[3].last())
            : radarObservationAt(m_catalogs[3], clock);
        if (!observation || m_failed[3]) {
            // A rolled-off observation is not a provider-wide outage. Later
            // frames can still use this healthy, cached primary catalog.
            job->failed = true;
            backups();
            return;
        }
        const auto source = WeatherRadarSource(WeatherRadarSource::Provider::LibreWxr).historicalFrame(observation->frameTime);
        QNetworkRequest request(source.imageUrl(job->bounds, job->size));
        auto* child = m_network->get(request);
        connect(reply, &QNetworkReply::finished, child, [child] { if (!child->isFinished()) { child->abort(); } });
        connect(child, &QNetworkReply::finished, this, [this, child, job, backups] {
            QByteArray bytes = child->error() == QNetworkReply::NoError ? child->read(16*1024*1024+1) : QByteArray{};
            child->deleteLater();
            QBuffer buffer(&bytes); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer,"PNG");
            if (!bytes.isEmpty() && bytes.size() <= 16*1024*1024 && reader.size() == job->size) { job->images[3] = reader.read(); }
            if (!job->images[3].isNull()) { m_primaryFallback = false; ++job->succeeded; finishImage(job); }
            else { m_primaryFallback = true; job->failed = true; backups(); }
        });
    }
    void exportBackups(const std::shared_ptr<Export>& job, int mask, const QDateTime& time, bool live)
    {
        if (!job->reply || job->reply->isFinished()) { complete(); return; }
        const auto reply = job->reply;
        QVector<QPair<int, WeatherRadarSource>> sources;
        for (int i = 0; i < 3; ++i) {
            if (!(mask & (1 << i))) { continue; }
            // Skip networks whose entire published region is outside this
            // request. This makes panning regional maps cheap.
            const QRectF footprint = i == 2 ? QRectF(-5009378,3503549,11131951,12035163)
                : i == 1 ? QRectF(-20037509,0,15584730,20037509) : QRectF(-20037509,0,40075018,20037509);
            if (!footprint.intersects(job->bounds)) { continue; }
            if (i == 0 && !QRectF(-20037509,0,13358340,20037509).intersects(job->bounds)
                && !QRectF(14471534,0,3896183,3503550).intersects(job->bounds)) { continue; }
            WeatherRadarSource source(static_cast<WeatherRadarSource::Provider>(i));
            if (!live) {
                const auto observation = radarObservationAt(m_catalogs[i], time);
                if (!observation) { job->failed = true; m_failed[i] = true; continue; }
                source = source.historicalFrame(observation->frameTime, observation->sampleTime, observation->rasterIds);
                if (i == 0) { job->noaaRasterIds = observation->rasterIds; }
            }
            sources.append({i,source});
        }
        job->remaining = sources.size();
        if (sources.isEmpty()) { finishImage(job); return; }
        for (const auto& entry : sources) {
            const int index = entry.first;
            QNetworkRequest request(entry.second.imageUrl(job->bounds,job->size));
            request.setTransferTimeout(30000);
            request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,QNetworkRequest::PreferCache);
            request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("AetherSDR (https://github.com/aethersdr/AetherSDR)"));
            QNetworkReply* child = m_network->get(request);
            connect(reply, &QNetworkReply::finished, child, [child] { if (!child->isFinished()) { child->abort(); } });
            connect(child, &QNetworkReply::downloadProgress, child, [child](qint64 n,qint64) { if(n>16*1024*1024) { child->abort(); } });
            connect(child, &QNetworkReply::finished, this, [this, child, job, index] {
                QByteArray bytes = child->error() == QNetworkReply::NoError ? child->read(16*1024*1024+1) : QByteArray{};
                child->deleteLater();
                QBuffer buffer(&bytes); buffer.open(QIODevice::ReadOnly);
                QImageReader reader(&buffer,"PNG");
                if (!bytes.isEmpty() && bytes.size() <= 16*1024*1024 && reader.size() == job->size) { job->images[index] = reader.read(); }
                if (index == 0 && !job->noaaRasterIds.isEmpty() && !job->images[index].isNull()
                    && job->reply && !job->reply->isFinished()) {
                    const QImage rgba = job->images[index].convertToFormat(QImage::Format_ARGB32);
                    bool visible = false;
                    for (int y = 0; !visible && y < rgba.height(); ++y) {
                        const auto* row = reinterpret_cast<const QRgb*>(rgba.constScanLine(y));
                        for (int x = 0; x < rgba.width(); ++x) { if (qAlpha(row[x]) != 0) { visible = true; break; } }
                    }
                    if (!visible) { verifyNoaa(job); return; }
                }
                settle(job, index, !job->images[index].isNull());
            });
        }
    }
    void settle(const std::shared_ptr<Export>& job, int index, bool valid)
    {
        m_failed[index] = !valid;
        if (!valid) { job->failed = true; job->images[index] = {}; }
        else { ++job->succeeded; }
        if (--job->remaining == 0) { finishImage(job); }
    }
    void verifyNoaa(const std::shared_ptr<Export>& job)
    {
        // Preserve the existing NOAA expired-raster guard. A transparent HTTP
        // 200 alone cannot distinguish clear weather from missing raster IDs.
        QNetworkRequest request(WeatherRadarSource::noaaRasterAvailabilityUrl(job->noaaRasterIds));
        request.setTransferTimeout(15000);
        QNetworkReply* reply = m_network->get(request);
        connect(job->reply, &QNetworkReply::finished, reply, [reply] { if (!reply->isFinished()) { reply->abort(); } });
        connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 n,qint64) { if (n > 512*1024) { reply->abort(); } });
        connect(reply, &QNetworkReply::finished, this, [this, reply, job] {
            const QByteArray bytes = reply->error() == QNetworkReply::NoError ? reply->read(512*1024+1) : QByteArray{};
            const auto available = bytes.size() <= 512*1024
                ? WeatherRadarSource::parseNoaaRasterAvailability(bytes,job->noaaRasterIds) : std::nullopt;
            reply->deleteLater();
            settle(job,0,available.value_or(false));
        });
    }
    void finishImage(const std::shared_ptr<Export>& job)
    {
        if (!job->reply || job->reply->isFinished()) { complete(); return; }
        if (job->failed && job->succeeded == 0) {
            job->reply->finish({}, QStringLiteral("Selected radar sources unavailable")); complete(); return;
        }
        auto* watcher = new QFutureWatcher<QByteArray>(this);
        connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [this, watcher, job] {
            const QByteArray png = watcher->result(); watcher->deleteLater();
            if (job->reply && !job->reply->isFinished()) {
                if (!job->failed) { m_cache.insert(job->reply->url().toString(), new QByteArray(png), png.size()/1024+1); }
                job->reply->finish(png);
            }
            complete();
        });
        watcher->setFuture(QtConcurrent::run([images = job->images, size = job->size] {
            const QImage image = composeRegionalRadar(images,size);
            QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); image.save(&buffer,"PNG"); return png;
        }));
    }
    QNetworkAccessManager* m_network;
    QCache<QString,QByteArray> m_cache;
    QVector<QPointer<CompositeReply>> m_queue;
    std::array<QVector<WeatherRadarObservation>,4> m_catalogs;
    std::array<QDateTime,4> m_catalogAt;
    std::array<QVector<std::function<void()>>,4> m_waiters;
    std::array<bool,4> m_failed{};
    bool m_primaryFallback{false};
    int m_active{0};
};
CompositeRenderer* renderer()
{
    static QPointer<CompositeRenderer> instance;
    if (!instance) { instance = new CompositeRenderer(QCoreApplication::instance()); }
    return instance;
}
}
QNetworkReply* requestRegionalRadar(const QNetworkRequest& request, QObject* parent) { return renderer()->request(request,parent); }
QString regionalRadarStatus(int enabledProviders) { return renderer()->status(enabledProviders); }
std::function<QNetworkReply*(const QNetworkRequest&, QObject*)> regionalRadarRequester(
    QNetworkAccessManager* network, QObject* context)
{
    const QPointer<CompositeRenderer> instance = new CompositeRenderer(context, network);
    return [instance](const QNetworkRequest& request, QObject* parent) -> QNetworkReply* {
        return instance ? instance->request(request, parent) : nullptr;
    };
}
}
