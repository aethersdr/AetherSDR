#include "gui/map/OperaRadarImage.h"
#include <QCoreApplication>
#include <QFile>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
using namespace AetherSDR;
namespace {
void put16(QByteArray& b, int at, quint16 v) { qToLittleEndian(v, b.data() + at); }
void put32(QByteArray& b, int at, quint32 v) { qToLittleEndian(v, b.data() + at); }
QByteArray shorts(std::initializer_list<quint16> values)
{
    QByteArray result(values.size() * 2, char(0));
    int i = 0;
    for (quint16 value : values) { put16(result, i, value); i += 2; }
    return result;
}
QByteArray doubles(std::initializer_list<double> values)
{
    QByteArray result(values.size() * 8, char(0));
    int i = 0;
    for (double value : values) {
        quint64 bits; std::memcpy(&bits, &value, 8);
        qToLittleEndian(bits, result.data() + i); i += 8;
    }
    return result;
}
// Synthetic, socket-free TIFF in the published profile. All 72 tiles reuse
// one compressed constant field, keeping the fixture small and deterministic.
QByteArray fixture()
{
    struct Tag { quint16 id, type; QByteArray data; };
    QVector<Tag> tags;
    const auto number = [&tags](quint16 tag, quint16 value) { tags.append({tag, 3, shorts({value})}); };
    number(256,3800); number(257,4400); number(258,32); number(259,8);
    number(277,2); number(284,1); number(317,1); number(322,512); number(323,512); number(339,3);
    tags.append({33550,12,doubles({1000,1000,0})});
    tags.append({33922,12,doubles({0,0,0,-500,500,0})});
    tags.append({34735,3,shorts({1,1,0,8, 3075,0,1,10, 3076,0,1,9001,
        3088,34736,1,0, 3089,34736,1,1, 3082,34736,1,2, 3083,34736,1,3,
        2057,34736,1,4, 2059,34736,1,5})});
    tags.append({34736,12,doubles({10,55,1950000,-2100000,6378137,298.257223563})});
    tags.append({324,4,QByteArray(72*4, char(0))});
    tags.append({325,4,QByteArray(72*4, char(0))});
    QByteArray b(8+2+tags.size()*12+4, char(0));
    b[0]='I'; b[1]='I'; put16(b,2,42); put32(b,4,8); put16(b,8,tags.size());
    int tileOffsets = 0, tileLengths = 0;
    for (int i=0;i<tags.size();++i) {
        const Tag& t=tags[i]; const int at=10+i*12;
        put16(b,at,t.id); put16(b,at+2,t.type);
        put32(b,at+4,t.data.size()/(t.type==3?2:t.type==4?4:8));
        if (t.data.size()<=4) { std::memcpy(b.data()+at+8,t.data.constData(),t.data.size()); }
        else {
            const int offset=b.size(); put32(b,at+8,offset); b.append(t.data);
            if (t.id==324) { tileOffsets=offset; }
            if (t.id==325) { tileLengths=offset; }
        }
    }
    QByteArray tile(512*512*8, char(0));
    const float value=20; quint32 bits; std::memcpy(&bits,&value,4);
    for (int i=0;i<512*512;++i) { put32(tile,i*8,bits); }
    const QByteArray compressed=qCompress(tile).mid(4);
    for (int i=0;i<72;++i) { put32(b,tileOffsets+i*4,b.size()); put32(b,tileLengths+i*4,compressed.size()); }
    b.append(compressed);
    return b;
}
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
