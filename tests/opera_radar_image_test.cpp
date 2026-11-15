#include "core/weather/OperaRadarImage.h"
#include "OperaRadarFixture.h"
#include <QCoreApplication>
#include <QFile>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
using namespace AetherSDR;
namespace {
using namespace OperaFixture;
QColor sample(const OperaRadarImage& raster, double lat, double lon)
{
    constexpr double pi=3.14159265358979323846, r=6378137;
    const double x=r*lon*pi/180, y=r*std::log(std::tan(pi/4+lat*pi/360));
    return raster.image.pixelColor(int((x-raster.bounds.left())/raster.bounds.width()*raster.image.width()),
        int((raster.bounds.bottom()-y)/raster.bounds.height()*raster.image.height()));
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    // Independent pyproj/PROJ 9.8.1 references, EPSG method 9820, WGS84.
    struct Reference { double lat, lon, x, y; };
    for (const Reference& ref : {Reference{55,10,1950000,-2100000},
        {51.5,-0.12,1249649.1633499577,-2439331.014665681},
        {60,25,2780384.198641504,-1452630.32471301},
        {40,-5,668193.0891402501,-3631649.4475884144},
        {70,30,2707283.288259018,-319041.7464096623}}) {
        const QPointF actual=OperaRadarImage::projectLaea(ref.lat,ref.lon);
        if (std::hypot(actual.x()-ref.x,actual.y()-ref.y)>0.01) { return 1; }
    }
    for (const auto& bytes : {QByteArray{}, QByteArray("II*\0garbage", 11), QByteArray(32,'x')}) {
        if (!OperaRadarImage::decode(bytes).image.isNull()) { return 2; }
    }
    QByteArray bytes=fixture();
    const OperaRadarImage raster=OperaRadarImage::decode(bytes);
    if (raster.image.isNull()) { std::cerr<<raster.error.toStdString()<<'\n'; return 3; }
    if (sample(raster,55,10).rgba()!=OperaRadarImage::colorForDbz(20).rgba()
        || sample(raster,51.5,-0.12).rgba()!=OperaRadarImage::colorForDbz(20).rgba()
        || sample(raster,31,-40).alpha()!=0) { return 4; }
    if (!OperaRadarImage::decode(bytes.left(bytes.size()-10)).image.isNull()) { return 5; }
    put16(bytes,10+3*12+8,5); // unsupported LZW compression must fail closed
    if (!OperaRadarImage::decode(bytes).image.isNull()) { return 6; }
    if (OperaRadarImage::colorForDbz(std::numeric_limits<float>::quiet_NaN()).alpha()!=0
        || OperaRadarImage::colorForDbz(-9999000).alpha()!=0) { return 7; }
    if (argc > 1) {
        QFile file(QString::fromLocal8Bit(argv[1]));
        if (!file.open(QIODevice::ReadOnly)) { return 8; }
        const OperaRadarImage actual=OperaRadarImage::decode(file.readAll());
        if (actual.image.isNull()) { std::cerr << actual.error.toStdString() << '\n'; return 9; }
        actual.image.save(QStringLiteral("/tmp/radar-opera-native.png"));
        std::cout << actual.image.width() << 'x' << actual.image.height() << '\n';
    }
    return 0;
}
