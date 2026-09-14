#pragma once
#include <QByteArray>
#include <QVector>
#include <QtEndian>
#include <cstring>
namespace OperaFixture {
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
    number(256,3800); number(257,4400); tags.append({258,3,shorts({32,32})}); number(259,8);
    number(277,2); number(284,1); number(317,1); number(322,512); number(323,512); tags.append({339,3,shorts({3,3})});
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
}
