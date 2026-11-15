#include "gui/map/LibreRadarNetwork.h"
#include "gui/map/RegionalRadarComposite.h"
#include "gui/map/WeatherRadarSource.h"
#include "gui/map/WeatherRadarViewGeometry.h"
#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkDiskCache>
#include <QTemporaryDir>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QTimeZone>
#include <QUrlQuery>
#include <cstring>
#include <limits>
using namespace AetherSDR;
namespace {
constexpr double extent = 20037508.342789244;
QByteArray png(const QSize& size, Qt::GlobalColor color)
{
    QImage image(size,QImage::Format_ARGB32_Premultiplied); image.fill(color);
    QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer,"PNG"); return bytes;
}
QByteArray metadata(qint64 stamp)
{
    return QJsonDocument(QJsonObject{{"host","https://api.librewxr.net"},
        {"radar",QJsonObject{{"past",QJsonArray{
            QJsonObject{{"time",stamp-600},{"path",QString("/v2/radar/%1").arg(stamp-600)}},
            QJsonObject{{"time",stamp},{"path",QString("/v2/radar/%1").arg(stamp)}}}},
            {"nowcast",QJsonArray{QJsonObject{{"time",stamp+600},{"path","/future"}}}}}}}).toJson();
}
class Reply final : public QNetworkReply {
public:
    Reply(const QNetworkRequest& request,QObject* parent,QByteArray bytes,bool fail,bool hold) : QNetworkReply(parent),m_bytes(std::move(bytes))
    {
        setRequest(request); setUrl(request.url()); open(QIODevice::ReadOnly);
        if (!hold) { QTimer::singleShot(0,this,[this,fail] {
            if (isFinished()) { return; }
            if(fail) { setError(TimeoutError,"injected outage"); m_bytes.clear(); }
            setFinished(true); emit readyRead(); emit finished();
        }); }
    }
    void abort() override { if(!isFinished()) { setError(OperationCanceledError,"canceled"); setFinished(true); emit finished(); } }
    qint64 bytesAvailable() const override { return m_bytes.size()-m_offset+QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char* data,qint64 size) override
    {
        const qint64 count=std::min(size,qint64(m_bytes.size())-m_offset);
        if(count<=0) { return -1; } std::memcpy(data,m_bytes.constData()+m_offset,size_t(count)); m_offset+=count; return count;
    }
private:
    QByteArray m_bytes; qint64 m_offset{0};
};
class Network final : public QNetworkAccessManager {
public:
    bool failPrimary{false}, clearPrimary{false}, holdTiles{false}, malformedTile{false}, coloredTiles{false};
    qint64 stamp=QDateTime::currentSecsSinceEpoch()/600*600-600;
    QList<QUrl> urls;
    QList<QPointer<Reply>> replies;
protected:
    QNetworkReply* createRequest(Operation,const QNetworkRequest& request,QIODevice*) override
    {
        const QUrl url=request.url(); urls.append(url);
        const bool catalog=url.path()=="/public/weather-maps.json";
        const bool primary=url.scheme()=="libre-radar" || url.host()=="api.librewxr.net";
        const QUrlQuery q(url);
        QSize size(256,256);
        if (url.host()=="api.librewxr.net" && !catalog) { size=QSize(512,512); }
        if (url.scheme()=="libre-radar") { size=QSize(q.queryItemValue("width").toInt(),q.queryItemValue("height").toInt()); }
        else if(q.hasQueryItem("size")) { const auto parts=q.queryItemValue("size").split(','); size=QSize(parts[0].toInt(),parts[1].toInt()); }
        QByteArray bytes=catalog ? metadata(stamp) : png(size,primary ? (clearPrimary ? Qt::transparent : Qt::green) : Qt::red);
        if (coloredTiles && !catalog && url.host()=="api.librewxr.net") {
            const auto parts=url.path().split('/');
            const Qt::GlobalColor colors[]={Qt::red,Qt::green,Qt::blue,Qt::yellow};
            bytes=png(size,colors[2*(parts.value(7).toInt()%2)+parts.value(6).toInt()%2]);
        }
        if(malformedTile && !catalog) { bytes=png(QSize(1024,1024),Qt::blue); }
        auto* reply=new Reply(request,this,bytes,primary&&!catalog&&failPrimary,holdTiles&&!catalog); replies.append(reply); return reply;
    }
};
}
class LibreRadarTest final : public QObject {
    Q_OBJECT
private slots:
    void timelineRejectsFutureStaleForeignAndFractional()
    {
        const WeatherRadarSource source(WeatherRadarSource::Provider::LibreWxr);
        const qint64 stamp=QDateTime::currentSecsSinceEpoch()-600;
        QCOMPARE(source.parseTimeline(metadata(stamp),2).size(),2);
        QVERIFY(source.parseTimeline(metadata(stamp+1800),2).isEmpty());
        QVERIFY(source.parseTimeline(metadata(stamp-3600),2).isEmpty());
        auto bad=metadata(stamp); bad.replace("https://api.librewxr.net","https://other.example");
        QVERIFY(source.parseTimeline(bad,2).isEmpty());
        bad=metadata(stamp); bad.replace(QByteArray::number(stamp)+"\n",QByteArray::number(stamp)+".5\n");
        QVERIFY(source.parseTimeline(bad,2).isEmpty());
        bad=metadata(stamp); bad.replace("/v2/radar/","/v2/other/");
        QVERIFY(source.parseTimeline(bad,2).isEmpty());
        QVERIFY(source.parseTimeline(QByteArray(512*1024+1,'x'),2).isEmpty());
    }
    void tileGeometryWrapsAndBoundsWork()
    {
        const auto plan=libreRadarTiles(QRectF(-extent,-extent,2*extent,2*extent),QSize(1024,1024),100);
        QCOMPARE(plan.size(),4);
        QCOMPARE(plan[0].destination,QRectF(0,0,512,512));
        QCOMPARE(plan[3].destination,QRectF(512,512,512,512));
        const auto east=libreRadarTiles(QRectF(extent,0,extent,extent),QSize(256,256),100);
        const auto west=libreRadarTiles(QRectF(-extent,0,extent,extent),QSize(256,256),100);
        QCOMPARE(east[0].url,west[0].url);
        const auto overview=libreRadarTiles(QRectF(-extent,-extent,2*extent,2*extent),QSize(2048,2048),100);
        QCOMPARE(overview.size(),16);
        QCOMPARE(overview[0].destination.size(),QSizeF(512,512));
        QVERIFY(overview[0].url.path().contains("/512/2/"));
        QVERIFY(libreRadarTiles(QRectF(0,0,std::numeric_limits<double>::infinity(),1),QSize(256,256),100).isEmpty());
        QVERIFY(libreRadarTiles(QRectF(0,0,1,1),QSize(4096,4096),100).isEmpty());
    }
    void primarySuccessAndClearPixelsNeverRequestBackups()
    {
        for (bool clear : {false,true}) {
            Network network; network.clearPrimary=clear; QObject context;
            const auto request=regionalRadarRequester(&network,&context);
            QNetworkReply* reply=request(QNetworkRequest(WeatherRadarSource::composite(15).tileUrl(3,2,3)),&context);
            QTRY_VERIFY(reply->isFinished());
            QCOMPARE(reply->error(),QNetworkReply::NoError);
            const QImage image=QImage::fromData(reply->readAll());
            QCOMPARE(image.pixelColor(100,100),QColor(clear ? Qt::transparent : Qt::green));
            QCOMPARE(network.urls.size(),2); // metadata + primary export, no backups
            QVERIFY(network.urls[1].scheme()=="libre-radar");
        }
    }
    void outageFallsBackAndDisabledPrimaryNeverContactsIt()
    {
        for (int mask : {9,1}) {
            Network network; network.failPrimary=true; QObject context;
            const auto request=regionalRadarRequester(&network,&context);
            QNetworkReply* reply=request(QNetworkRequest(WeatherRadarSource::composite(mask).tileUrl(3,2,3)),&context);
            QTRY_VERIFY(reply->isFinished());
            QCOMPARE(reply->error(),QNetworkReply::NoError);
            QCOMPARE(QImage::fromData(reply->readAll()).pixelColor(100,100),QColor(Qt::red));
            QCOMPARE(network.urls.size(),mask==9 ? 3 : 1);
            QCOMPARE(network.urls.last().host(),QString("mapservices.weather.noaa.gov"));
        }
    }
    void primaryTimelineDoesNotDownloadBackupCatalogs()
    {
        Network network; QObject context; const auto request=regionalRadarRequester(&network,&context);
        QNetworkReply* reply=request(QNetworkRequest(WeatherRadarSource::composite(15).timelineUrl()),&context);
        QTRY_VERIFY(reply->isFinished());
        QCOMPARE(network.urls.size(),1);
        const QByteArray bytes = reply->readAll();
        const auto configured = WeatherRadarSource::composite(15);
        QCOMPARE(configured.parseTimeline(bytes,4).size(),2);
        QCOMPARE(configured.playbackSourceForTimeline(bytes).enabledProviders(),8);
    }
    void playbackFailureDoesNotBecomeRegionalOnlyFrame()
    {
        Network network; QObject context;
        const auto request = regionalRadarRequester(&network, &context);
        const auto configured = WeatherRadarSource::composite(15);
        auto* catalog = request(QNetworkRequest(configured.timelineUrl()), &context);
        QTRY_VERIFY(catalog->isFinished());
        const auto playback = configured.playbackSourceForTimeline(catalog->readAll());
        network.failPrimary = true;
        auto* missing = request(QNetworkRequest(playback.historicalFrame(
            QDateTime::fromSecsSinceEpoch(network.stamp, QTimeZone::UTC)).tileUrl(3, 2, 3)), &context);
        QTRY_VERIFY(missing->isFinished());
        QVERIFY(missing->error() != QNetworkReply::NoError);
        QVERIFY(missing->readAll().isEmpty()); // Controller retains the complete original.
        QCOMPARE(network.urls.size(), 2); // Catalog + primary, no regional replacement.
        network.failPrimary = false;
        auto* next = request(QNetworkRequest(playback.historicalFrame(
            QDateTime::fromSecsSinceEpoch(network.stamp - 600, QTimeZone::UTC)).tileUrl(3, 2, 3)), &context);
        QTRY_VERIFY(next->isFinished());
        QCOMPARE(next->error(), QNetworkReply::NoError);
        QCOMPARE(QImage::fromData(next->readAll()).pixelColor(100,100), QColor(Qt::green));
        const auto fallback = configured.playbackSourceForTimeline(R"({"providers":7})");
        QCOMPARE(fallback.enabledProviders(), 7); // A fallback movie stays regional.
        QCOMPARE(configured.playbackSourceForTimeline(R"({"providers":15})").enabledProviders(), 8);
        QCOMPARE(WeatherRadarSource::composite(1).playbackSourceForTimeline(R"({"providers":8})").enabledProviders(), 1);
    }
    void missingOldFrameDoesNotDisableHealthyPrimary()
    {
        Network network; QObject context;
        const auto request = regionalRadarRequester(&network, &context);
        const auto primary = WeatherRadarSource::composite(8);
        auto* old = request(QNetworkRequest(primary.historicalFrame(
            QDateTime::fromSecsSinceEpoch(network.stamp - 3600, QTimeZone::UTC)).tileUrl(2, 1, 1)), &context);
        QTRY_VERIFY(old->isFinished());
        QVERIFY(old->error() != QNetworkReply::NoError);
        auto* current = request(QNetworkRequest(primary.tileUrl(2, 1, 1)), &context);
        QTRY_VERIFY(current->isFinished());
        QCOMPARE(current->error(), QNetworkReply::NoError);
    }
    void paddedViewFitsNativeExportBudget()
    {
        const QRectF bounds(-8000000, 3000000, 3000000, 3000000);
        const WeatherRadarViewGeometry view{bounds, weatherRadarLimitedSize(QSize(4096, 4096))};
        const auto padded = weatherRadarPaddedView(view, 4096, kMaximumWeatherRadarPixels);
        QVERIFY(padded.covers(view));
        QVERIFY(qint64(padded.size.width()) * padded.size.height() <= kMaximumWeatherRadarPixels);
        Network network; QObject context;
        const auto request = regionalRadarRequester(&network, &context);
        auto* reply = request(QNetworkRequest(WeatherRadarSource::composite(1).imageUrl(
            padded.bounds, padded.size)), &context);
        QTRY_VERIFY(reply->isFinished());
        QCOMPARE(reply->error(), QNetworkReply::NoError);
        QCOMPARE(weatherRadarLimitedSize(QSize(4096, 1024)), QSize(4096, 1024));
    }
    void canceledPrimaryDoesNotStartBackups()
    {
        Network network; network.holdTiles=true; QObject context; const auto request=regionalRadarRequester(&network,&context);
        auto* reply=request(QNetworkRequest(WeatherRadarSource::composite(15).tileUrl(3,2,3)),&context);
        QTRY_COMPARE(network.urls.size(),2);
        reply->abort(); QCoreApplication::processEvents();
        QCOMPARE(network.urls.size(),2);
        QCOMPARE(network.replies.last()->error(),QNetworkReply::OperationCanceledError);
    }
    void tiledRendererCachesAndRejectsMalformedImages()
    {
        for (bool malformed : {false,true}) {
            Network network; network.malformedTile=malformed; LibreRadarRenderer renderer(&network);
            const auto source=WeatherRadarSource(WeatherRadarSource::Provider::LibreWxr).historicalFrame(QDateTime::fromSecsSinceEpoch(network.stamp,QTimeZone::UTC));
            const QNetworkRequest request(source.imageUrl(QRectF(-extent,-extent,2*extent,2*extent),QSize(1024,1024)));
            auto* reply=renderer.request(request,&renderer); QTRY_VERIFY(reply->isFinished());
            if(malformed) { QVERIFY(reply->error()!=QNetworkReply::NoError); continue; }
            const auto image=QImage::fromData(reply->readAll()); QCOMPARE(image.size(),QSize(1024,1024));
            QCOMPARE(image.pixelColor(400,400),QColor(Qt::green));
            QCOMPARE(network.urls.size(),4);
            reply=renderer.request(request,&renderer); QTRY_VERIFY(reply->isFinished());
            QCOMPARE(reply->error(),QNetworkReply::NoError); QCOMPARE(network.urls.size(),4);
        }
    }
    void tileExportsDoNotFetchRoundingNeighbours()
    {
        Network network; LibreRadarRenderer renderer(&network);
        const auto source=WeatherRadarSource(WeatherRadarSource::Provider::LibreWxr).historicalFrame(QDateTime::fromSecsSinceEpoch(network.stamp,QTimeZone::UTC));
        for (int zoom=0; zoom<=8; ++zoom) {
            const int index=(1<<zoom)-1;
            auto* reply=renderer.request(QNetworkRequest(source.tileUrl(zoom,index,index)),&renderer);
            QTRY_VERIFY(reply->isFinished());
            QCOMPARE(reply->error(),QNetworkReply::NoError);
            QCOMPARE(network.urls.size(),zoom+1);
            QCOMPARE(network.urls.last().path(),QString("/v2/radar/%1/512/%2/%3/%3/6/0_0.png").arg(network.stamp).arg(zoom).arg(index));
        }
    }
    void zoomOutAndBackReuseDetailAcrossRestart()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Network network; network.coloredTiles=true;
        auto* cache=new QNetworkDiskCache(&network); cache->setCacheDirectory(directory.path()); network.setCache(cache);
        const auto source=WeatherRadarSource(WeatherRadarSource::Provider::LibreWxr).historicalFrame(QDateTime::fromSecsSinceEpoch(network.stamp,QTimeZone::UTC));
        const QRectF world(-extent,-extent,2*extent,2*extent);
        const QNetworkRequest detail(source.imageUrl(world,QSize(1024,1024)));
        const QNetworkRequest overview(source.imageUrl(world,QSize(512,512)));
        {
            LibreRadarRenderer renderer(&network);
            auto* reply=renderer.request(detail,&renderer); QTRY_VERIFY(reply->isFinished());
            QCOMPARE(reply->error(),QNetworkReply::NoError); QCOMPARE(network.urls.size(),4);
        }
        // A new renderer has no RAM cache. It must find the four neighbours on
        // disk and create a lower-zoom image locally, including clear tiles.
        LibreRadarRenderer restarted(&network);
        auto* reply=restarted.request(overview,&restarted); QTRY_VERIFY(reply->isFinished());
        QCOMPARE(reply->error(),QNetworkReply::NoError); QCOMPARE(network.urls.size(),4);
        const QImage image=QImage::fromData(reply->readAll());
        QCOMPARE(image.pixelColor(128,128),QColor(Qt::red));
        QCOMPARE(image.pixelColor(384,128),QColor(Qt::green));
        QCOMPARE(image.pixelColor(128,384),QColor(Qt::blue));
        QCOMPARE(image.pixelColor(384,384),QColor(Qt::yellow));
        reply=restarted.request(detail,&restarted); QTRY_VERIFY(reply->isFinished());
        QCOMPARE(reply->error(),QNetworkReply::NoError); QCOMPARE(network.urls.size(),4);
        // Zooming in beyond cached detail must download real detail, never
        // substitute an enlarged parent, and new clocks have separate keys.
        reply=restarted.request(QNetworkRequest(source.tileUrl(2,0,0)),&restarted);
        QTRY_VERIFY(reply->isFinished()); QCOMPARE(network.urls.size(),5);
        const auto newer=source.historicalFrame(QDateTime::fromSecsSinceEpoch(network.stamp+600,QTimeZone::UTC));
        reply=restarted.request(QNetworkRequest(newer.imageUrl(world,QSize(512,512))),&restarted);
        QTRY_VERIFY(reply->isFinished()); QCOMPARE(network.urls.size(),6);
    }
    void diskCacheValidatesPixelsAndPreservesClearWeather()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Network network; network.clearPrimary=true;
        auto* cache=new QNetworkDiskCache(&network); cache->setCacheDirectory(directory.path()); network.setCache(cache);
        const auto source=WeatherRadarSource(WeatherRadarSource::Provider::LibreWxr).historicalFrame(QDateTime::fromSecsSinceEpoch(network.stamp,QTimeZone::UTC));
        const QNetworkRequest request(source.tileUrl(0,0,0));
        {
            LibreRadarRenderer renderer(&network);
            auto* reply=renderer.request(request,&renderer); QTRY_VERIFY(reply->isFinished());
            QCOMPARE(reply->error(),QNetworkReply::NoError); QCOMPARE(network.urls.size(),1);
        }
        const QUrl url=network.urls[0];
        QVERIFY(cache->metaData(url).expirationDate()>QDateTime::currentDateTimeUtc().addSecs(3600));
        {
            LibreRadarRenderer renderer(&network);
            auto* reply=renderer.request(request,&renderer); QTRY_VERIFY(reply->isFinished());
            QCOMPARE(reply->error(),QNetworkReply::NoError); QCOMPARE(network.urls.size(),1);
            QCOMPARE(QImage::fromData(reply->readAll()).pixelColor(100,100),QColor(Qt::transparent));
        }
        auto meta=cache->metaData(url);
        QIODevice* output=cache->prepare(meta); QVERIFY(output);
        output->write("corrupted PNG"); cache->insert(output);
        {
            LibreRadarRenderer renderer(&network);
            auto* reply=renderer.request(request,&renderer); QTRY_VERIFY(reply->isFinished());
            QCOMPARE(reply->error(),QNetworkReply::NoError); QCOMPARE(network.urls.size(),2);
        }
        meta=cache->metaData(url); meta.setExpirationDate(QDateTime::currentDateTimeUtc().addSecs(-1));
        cache->updateMetaData(meta);
        LibreRadarRenderer renderer(&network);
        auto* reply=renderer.request(request,&renderer); QTRY_VERIFY(reply->isFinished());
        QCOMPARE(reply->error(),QNetworkReply::NoError); QCOMPARE(network.urls.size(),3);
    }
    void sharedTileOnlyCancelsAfterLastConsumer()
    {
        Network network; network.holdTiles=true; LibreRadarRenderer renderer(&network);
        const auto source=WeatherRadarSource(WeatherRadarSource::Provider::LibreWxr).historicalFrame(QDateTime::fromSecsSinceEpoch(network.stamp,QTimeZone::UTC));
        const QNetworkRequest request(source.tileUrl(0,0,0));
        auto* first=renderer.request(request,&renderer); auto* second=renderer.request(request,&renderer);
        QTRY_COMPARE(network.urls.size(),1);
        first->abort(); QVERIFY(!network.replies[0]->isFinished());
        second->abort(); QVERIFY(network.replies[0]->isFinished());
        QCOMPARE(network.replies[0]->error(),QNetworkReply::OperationCanceledError);
    }
};
QTEST_GUILESS_MAIN(LibreRadarTest)
#include "libre_radar_test.moc"
