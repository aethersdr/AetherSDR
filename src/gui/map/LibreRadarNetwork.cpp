#include "LibreRadarNetwork.h"
#include "MapProviderNetworkAccessManager.h"
#include "WeatherRadarViewGeometry.h"
#include <QBuffer>
#include <QCache>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QHash>
#include <QImageReader>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QPainter>
#include <QPointer>
#include <QStandardPaths>
#include <QTimer>
#include <QTimeZone>
#include <QUrlQuery>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace AetherSDR {
namespace {
constexpr double kExtent = 20037508.342789244;
constexpr int kTileSize = 512;
constexpr int kMaximumTileBytes = 2*1024*1024;
constexpr int kMaximumTiles = 32;
bool validExport(const QRectF& bounds, const QSize& size)
{
    return std::isfinite(bounds.left()) && std::isfinite(bounds.right())
        && std::isfinite(bounds.top()) && std::isfinite(bounds.bottom())
        && bounds.width() > 0 && bounds.height() > 0
        && std::abs(bounds.left()) <= 2*kExtent && std::abs(bounds.right()) <= 2*kExtent
        && std::abs(bounds.top()) <= 2*kExtent && std::abs(bounds.bottom()) <= 2*kExtent
        && size.width() > 0 && size.height() > 0 && size.width() <= 4096 && size.height() <= 4096
        && qint64(size.width())*size.height() <= kMaximumWeatherRadarPixels;
}
// Export URLs round Mercator bounds to millimetres. Snap that rounding error
// at XYZ boundaries so a single tile never fans out into neighbouring tiles.
double snapTileBoundary(double value)
{
    const double rounded = std::round(value);
    return std::abs(value-rounded) < 1e-7 ? rounded : value;
}
QImage decodeTile(QByteArray bytes)
{
    if (bytes.isEmpty() || bytes.size() > kMaximumTileBytes) { return {}; }
    QBuffer buffer(&bytes); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer,"PNG");
    return reader.size() == QSize(kTileSize,kTileSize) ? reader.read() : QImage{};
}
class LibreReply final : public QNetworkReply {
public:
    LibreReply(const QNetworkRequest& request, QObject* parent) : QNetworkReply(parent)
    {
        setRequest(request); setUrl(request.url()); setOperation(QNetworkAccessManager::GetOperation);
        open(QIODevice::ReadOnly);
    }
    void abort() override { finish({}, OperationCanceledError); }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return m_bytes.size()-m_offset+QNetworkReply::bytesAvailable(); }
    void finish(const QByteArray& bytes, NetworkError error = NoError)
    {
        if (isFinished()) { return; }
        m_bytes = bytes;
        if (error != NoError) { setError(error, QStringLiteral("LibreWXR precipitation unavailable")); }
        setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("image/png"));
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, error == NoError ? 200 : 503);
        setFinished(true);
        if (error != NoError) { emit errorOccurred(error); }
        if (!bytes.isEmpty()) { emit readyRead(); }
        emit finished();
    }
protected:
    qint64 readData(char* target, qint64 count) override
    {
        count = std::min(count, qint64(m_bytes.size())-m_offset);
        if (count <= 0) { return -1; }
        std::memcpy(target,m_bytes.constData()+m_offset,size_t(count)); m_offset += count; return count;
    }
private:
    QByteArray m_bytes;
    qint64 m_offset{0};
};
struct Job {
    QPointer<LibreReply> reply;
    QImage image;
    int remaining{0};
};
struct Consumer {
    std::shared_ptr<Job> job;
    QRectF destination;
};
}

QVector<LibreRadarTile> libreRadarTiles(const QRectF& bounds, const QSize& size, qint64 timestamp)
{
    if (!validExport(bounds,size) || timestamp <= 0) { return {}; }
    // Cap zoom and requests even for large playback exports. No full-world
    // prefetch: each call covers just this export at its display resolution.
    const double density = std::max(size.width()/bounds.width(), size.height()/bounds.height());
    const double level = std::ceil(std::log2(2*kExtent*density/kTileSize)-1e-7);
    int zoom = int(std::clamp(level,0.0,8.0));
    int x0, x1, y0, y1;
    double span;
    int count;
    do {
        count = 1 << zoom;
        span = 2*kExtent/count;
        x0 = int(std::floor(snapTileBoundary((bounds.left()+kExtent)/span)));
        x1 = int(std::ceil(snapTileBoundary((bounds.right()+kExtent)/span)))-1;
        y0 = std::max(0,int(std::floor(snapTileBoundary((kExtent-bounds.bottom())/span))));
        y1 = std::min(count-1,int(std::ceil(snapTileBoundary((kExtent-bounds.top())/span)))-1);
        if ((x1-x0+1)*(y1-y0+1) <= kMaximumTiles || zoom == 0) { break; }
        --zoom;
    } while (true);
    QVector<LibreRadarTile> tiles;
    for (int y=y0; y<=y1; ++y) {
        for (int x=x0; x<=x1; ++x) {
            const int wrapped = ((x%count)+count)%count;
            const QUrl url(QStringLiteral("https://api.librewxr.net/v2/radar/%1/512/%2/%3/%4/6/0_0.png")
                .arg(timestamp).arg(zoom).arg(wrapped).arg(y));
            const QRectF destination((x*span-kExtent-bounds.left())/bounds.width()*size.width(),
                (bounds.bottom()-(kExtent-y*span))/bounds.height()*size.height(),
                span/bounds.width()*size.width(),span/bounds.height()*size.height());
            tiles.append({url,destination});
        }
    }
    return tiles;
}

class LibreRadarRenderer::Impl final : public QObject {
public:
    Impl(QNetworkAccessManager* network, QObject* parent) : QObject(parent), m_network(network), m_cache(64*1024) {}
    QNetworkReply* request(const QNetworkRequest& request, QObject* parent)
    {
        auto* reply = new LibreReply(request,parent);
        QTimer::singleShot(0,this,[this,reply=QPointer<LibreReply>(reply)] {
            if (!reply || reply->isFinished()) { return; }
            const QUrlQuery query(reply->url());
            const auto values = query.queryItemValue(QStringLiteral("bbox")).split(',');
            double v[4]{}; bool valid = values.size()==4;
            for (int i=0; valid && i<4; ++i) { bool ok; v[i]=values[i].toDouble(&ok); valid=ok; }
            const QRectF bounds(QPointF(v[0],v[1]),QPointF(v[2],v[3]));
            const QSize size(query.queryItemValue(QStringLiteral("width")).toInt(),query.queryItemValue(QStringLiteral("height")).toInt());
            bool ok;
            const qint64 stamp=query.queryItemValue(QStringLiteral("time")).toLongLong(&ok);
            const qint64 now=QDateTime::currentSecsSinceEpoch();
            if (!valid || !ok || !validExport(bounds,size) || stamp < now-5*3600 || stamp > now) {
                reply->finish({},QNetworkReply::ProtocolInvalidOperationError); return;
            }
            const auto plan=libreRadarTiles(bounds,size,stamp);
            if (plan.isEmpty() || m_consumers.size()+plan.size()>128) {
                reply->finish({},QNetworkReply::TemporaryNetworkFailureError); return;
            }
            auto job=std::make_shared<Job>(); job->reply=reply;
            job->image=QImage(size,QImage::Format_ARGB32_Premultiplied); job->image.fill(Qt::transparent);
            job->remaining=plan.size();
            for (const auto& tile : plan) {
                const QString key=tile.url.toString();
                const QImage cached=cachedTile(tile.url,2);
                if (!cached.isNull()) { deliver({job,tile.destination},cached); continue; }
                if (!m_consumers.contains(key)) { m_queue.append(key); }
                m_consumers[key].append({job,tile.destination});
            }
            connect(reply,&QNetworkReply::finished,this,[this] { retireCanceled(); });
            pump();
        });
        return reply;
    }
private:
    void rememberTile(const QUrl& url, const QImage& image, QByteArray bytes = {})
    {
        m_cache.insert(url.toString(),new QImage(image),int(image.sizeInBytes()/1024)+1);
        auto* disk=m_network->cache();
        if (!disk) { return; }
        if (bytes.isEmpty()) {
            QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
            if (!image.save(&buffer,"PNG")) { return; }
        }
        // Timestamped tiles are local frame snapshots. Keep them for the same
        // five-hour admission window across zoom changes and app restarts.
        // Live discovery still polls for new timestamps every five minutes.
        const qint64 stamp=url.path().split('/').value(3).toLongLong();
        QNetworkCacheMetaData metadata;
        metadata.setUrl(url);
        metadata.setExpirationDate(QDateTime::fromSecsSinceEpoch(stamp+5*3600,QTimeZone::UTC));
        metadata.setLastModified(QDateTime::currentDateTimeUtc());
        metadata.setRawHeaders({{"Content-Type","image/png"}});
        metadata.setSaveToDisk(true);
        if (QIODevice* output=disk->prepare(metadata)) {
            if (output->write(bytes)==bytes.size()) { disk->insert(output); }
            else { disk->remove(url); }
        }
    }
    QImage cachedTile(const QUrl& url, int childDepth)
    {
        if (const QImage* image=m_cache.object(url.toString())) { return *image; }
        auto* disk=m_network->cache();
        if (disk) {
            const auto metadata=disk->metaData(url);
            if (metadata.isValid()) {
                if (metadata.expirationDate()>QDateTime::currentDateTimeUtc()) {
                    std::unique_ptr<QIODevice> data(disk->data(url));
                    const QImage image=data ? decodeTile(data->read(kMaximumTileBytes+1)) : QImage{};
                    if (!image.isNull()) {
                        m_cache.insert(url.toString(),new QImage(image),int(image.sizeInBytes()/1024)+1);
                        return image;
                    }
                }
                disk->remove(url);
            }
        }
        // Zooming out can reuse four higher-detail neighbours. Never upscale
        // a parent in place of downloading detail when the user zooms in.
        QStringList parts=url.path().split('/');
        const int zoom=parts.value(5).toInt();
        if (childDepth<=0 || zoom>=8) { return {}; }
        const int x=parts.value(6).toInt(), y=parts.value(7).toInt();
        QImage parent(kTileSize,kTileSize,QImage::Format_ARGB32_Premultiplied);
        parent.fill(Qt::transparent);
        QPainter painter(&parent);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        for (int dy=0; dy<2; ++dy) {
            for (int dx=0; dx<2; ++dx) {
                parts[5]=QString::number(zoom+1);
                parts[6]=QString::number(2*x+dx); parts[7]=QString::number(2*y+dy);
                QUrl child=url; child.setPath(parts.join('/'));
                const QImage image=cachedTile(child,childDepth-1);
                if (image.isNull()) { return {}; }
                painter.drawImage(QRect(dx*kTileSize/2,dy*kTileSize/2,kTileSize/2,kTileSize/2),image);
            }
        }
        painter.end();
        rememberTile(url,parent);
        return parent;
    }
    void deliver(const Consumer& consumer, const QImage& image)
    {
        const auto& job=consumer.job;
        if (!job->reply || job->reply->isFinished()) { return; }
        if (image.isNull()) { job->reply->finish({},QNetworkReply::TemporaryNetworkFailureError); return; }
        {
            QPainter painter(&job->image);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(consumer.destination,image);
        }
        if (--job->remaining==0) {
            QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
            if (!job->image.save(&buffer,"PNG")) { job->reply->finish({},QNetworkReply::UnknownContentError); return; }
            job->reply->finish(bytes);
        }
    }
    void retireCanceled()
    {
        // Abort a shared download only after its last consumer retires.
        const auto keys=m_consumers.keys();
        for (const QString& key : keys) {
            auto& consumers=m_consumers[key];
            consumers.erase(std::remove_if(consumers.begin(),consumers.end(),[](const auto& c) {
                return !c.job->reply || c.job->reply->isFinished();
            }),consumers.end());
            if (consumers.isEmpty()) {
                if (auto reply=m_active.value(key)) { reply->abort(); }
            }
        }
    }
    void pump()
    {
        while (m_active.size()<4 && !m_queue.isEmpty()) {
            const QString key=m_queue.takeFirst();
            if (m_consumers.value(key).isEmpty()) { m_consumers.remove(key); continue; }
            QNetworkRequest request{QUrl(key)};
            request.setTransferTimeout(20000);
            // Validated PNGs are persisted explicitly with frame lifetime below.
            request.setAttribute(QNetworkRequest::CacheSaveControlAttribute,false);
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::SameOriginRedirectPolicy);
            request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,QNetworkRequest::PreferCache);
            request.setHeader(QNetworkRequest::UserAgentHeader,QStringLiteral("AetherSDR (https://github.com/aethersdr/AetherSDR)"));
            auto* reply=m_network->get(request); m_active.insert(key,reply);
            reply->setReadBufferSize(kMaximumTileBytes+1);
            connect(reply,&QNetworkReply::readyRead,reply,[reply] { if(reply->bytesAvailable()>kMaximumTileBytes) { reply->abort(); } });
            connect(reply,&QNetworkReply::finished,this,[this,reply,key] {
                QByteArray bytes;
                if (reply->error()==QNetworkReply::NoError) { bytes=reply->read(kMaximumTileBytes+1); }
                const QImage image=decodeTile(bytes);
                if (!image.isNull()) { rememberTile(QUrl(key),image,bytes); }
                const auto consumers=m_consumers.take(key); m_active.remove(key); reply->deleteLater();
                for (const auto& consumer : consumers) { deliver(consumer,image); }
                QTimer::singleShot(0,this,[this] { pump(); });
            });
        }
    }
    QNetworkAccessManager* m_network;
    QCache<QString,QImage> m_cache;
    QHash<QString,QVector<Consumer>> m_consumers;
    QHash<QString,QPointer<QNetworkReply>> m_active;
    QStringList m_queue;
};
LibreRadarRenderer::LibreRadarRenderer(QNetworkAccessManager* network, QObject* parent)
    : QObject(parent),m_impl(std::make_unique<Impl>(network,this)) {}
LibreRadarRenderer::~LibreRadarRenderer() = default;
QNetworkReply* LibreRadarRenderer::request(const QNetworkRequest& request, QObject* parent) { return m_impl->request(request,parent); }
QNetworkReply* requestLibreRadar(const QNetworkRequest& request, QObject* parent)
{
    static QPointer<LibreRadarRenderer> renderer;
    if (!renderer) {
        auto* network=new MapProviderNetworkAccessManager(QCoreApplication::instance());
        auto* cache=new QNetworkDiskCache(network);
        cache->setCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)+QStringLiteral("/librewxr"));
        cache->setMaximumCacheSize(256*1024*1024); network->setCache(cache);
        renderer=new LibreRadarRenderer(network,network);
    }
    return renderer->request(request,parent);
}
}
