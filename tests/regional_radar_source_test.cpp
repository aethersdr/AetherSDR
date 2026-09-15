#include "gui/map/WeatherRadarSource.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>
#include <QUrlQuery>
#include <iostream>
using namespace AetherSDR;
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto check = [](bool ok, const char* label) { if (!ok) { std::cerr << label << '\n'; std::exit(1); } };
    const WeatherRadarSource canada(WeatherRadarSource::Provider::Eccc);
    const QDateTime now = QDateTime::fromSecsSinceEpoch(QDateTime::currentSecsSinceEpoch()-600, QTimeZone::UTC);
    const auto xml = QStringLiteral("<WMS_Capabilities><Layer><Name>RADAR_1KM_RRAI</Name><Dimension name='time'>%1/%2/PT6M</Dimension></Layer></WMS_Capabilities>")
        .arg(now.addSecs(-3*3600).toString(Qt::ISODate), now.toString(Qt::ISODate)).toUtf8();
    const auto frames = canada.parseTimeline(xml, 1);
    check(frames.size()==11 && frames.last().frameTime==now, "Canada temporal extent");
    check(canada.parseTimeline(xml + "<broken>", 1).isEmpty(), "malformed capabilities");
    auto wrongScope = xml;
    wrongScope.replace("<Dimension", "</Layer><Layer><Dimension");
    check(canada.parseTimeline(wrongScope,1).isEmpty(),"time cannot leak into an unnamed sibling layer");
    auto bad=xml; bad.replace("PT6M","PT0M");
    check(canada.parseTimeline(bad, 1).isEmpty(), "zero cadence rejection");
    const auto source = canada.historicalFrame(now);
    QUrlQuery query(source.tileUrl(3,2,3));
    check(query.queryItemValue("CRS")=="EPSG:3857", "WMS CRS");
    check(query.queryItemValue("TIME")==now.toString(Qt::ISODate), "exact WMS time");
    check(source.tileUrl(3,-1,3)==source.tileUrl(3,7,3), "Canada world wrapping");
    check(source.frameId()!=WeatherRadarSource::historicalNoaaFrame(now).frameId(), "cache identity separates products");
    const WeatherRadarSource europe(WeatherRadarSource::Provider::Opera);
    const QDateTime stamp=QDateTime::fromSecsSinceEpoch(now.toSecsSinceEpoch()/300*300,QTimeZone::UTC);
    QJsonArray links;
    links.append(QJsonObject{{"href",WeatherRadarSource::operaFrameUrl(stamp).toString()}});
    links.append(QJsonObject{{"href","https://untrusted.example/OPERA@20260910T0000@0@DBZH.tiff"}});
    const auto data=QJsonDocument(QJsonObject{{"links",links}}).toJson();
    const auto euFrames=europe.parseTimeline(data,1);
    check(euFrames.size()==1 && euFrames[0].frameTime==stamp,"OPERA published file identity and host validation");
    check(europe.imageUrl(QRectF(0,0,100,100),QSize(256,256)).scheme()=="opera-radar","native European renderer");
    const auto combined = WeatherRadarSource::composite(5);
    check(combined.latestFrame().enabledProviders()==5,"live refresh preserves selected regions");
    check(combined.historicalFrame(stamp).enabledProviders()==5,"playback preserves selected regions");
    check(combined.frameId()!=WeatherRadarSource::composite(7).frameId(),"different region masks cannot share cached pixels");
    const auto timeline=QJsonDocument(QJsonObject{{"times",QJsonArray{stamp.addSecs(-300).toSecsSinceEpoch(),stamp.toSecsSinceEpoch()}}}).toJson();
    check(combined.parseTimeline(timeline,1).size()==2,"combined playback clocks");
    check(combined.parseTimeline(QJsonDocument(QJsonObject{{"times",QJsonArray{stamp.toSecsSinceEpoch(),stamp.toSecsSinceEpoch()}}}).toJson(),1).isEmpty(),"duplicate combined clocks rejected");
    return 0;
}
