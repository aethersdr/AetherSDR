#include "core/weather/OperaRadarImage.h"
#include "OperaRadarFixture.h"
#include <QCoreApplication>
#include <iostream>
#include <functional>
using namespace AetherSDR;
using namespace OperaFixture;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QByteArray valid = fixture();
    const auto tagAt = [](const QByteArray& b, quint16 wanted) {
        const int count = qFromLittleEndian<quint16>(b.constData() + 8);
        for (int i = 0; i < count; ++i) {
            const int at = 10 + i * 12;
            if (qFromLittleEndian<quint16>(b.constData() + at) == wanted) { return at; }
        }
        return -1;
    };
    struct Mutation { const char* name; std::function<void(QByteArray&)> apply; };
    const QVector<Mutation> corpus{
        {"looping IFD", [](QByteArray& b) { put32(b, 10 + 16 * 12, 8); }},
        {"out-of-buffer IFD", [](QByteArray& b) { put32(b, 4, 0xfffffff0); }},
        {"missing next-IFD word", [](QByteArray& b) { b.truncate(10 + 16 * 12); }},
        {"header IFD", [](QByteArray& b) { put32(b, 4, 2); }},
        {"oversized IFD", [](QByteArray& b) { put16(b, 8, 65535); }},
        {"zero IFD", [](QByteArray& b) { put32(b, 4, 0); }},
        {"duplicate tag", [](QByteArray& b) { put16(b, 22, 256); }},
        {"huge width", [=](QByteArray& b) { const int a=tagAt(b,256); put16(b,a+2,4); put32(b,a+8,0xffffffff); }},
        {"huge count", [=](QByteArray& b) { put32(b,tagAt(b,324)+4,0xffffffff); }},
        {"bad tile offset", [=](QByteArray& b) { const int a=tagAt(b,324); const int off=qFromLittleEndian<quint32>(b.constData()+a+8); put32(b,off,0xfffffff0); }},
        {"bad tile length", [=](QByteArray& b) { const int a=tagAt(b,325); const int off=qFromLittleEndian<quint32>(b.constData()+a+8); put32(b,off,0xffffffff); }},
        {"unsupported compression", [=](QByteArray& b) { put16(b,tagAt(b,259)+8,5); }},
        {"mismatched second sample", [=](QByteArray& b) { put16(b,tagAt(b,258)+10,64); }},
        {"mismatched second format", [=](QByteArray& b) { put16(b,tagAt(b,339)+10,1); }},
        {"unsupported SubIFDs", [=](QByteArray& b) { put16(b,tagAt(b,33550),330); }},
        {"truncated deflate", [](QByteArray& b) { b.chop(8); }},
        {"excess decompressed output", [=](QByteArray& b) {
            const QByteArray payload=qCompress(QByteArray(512*512*8+1, '\0')).mid(4);
            const int offsets=qFromLittleEndian<quint32>(b.constData()+tagAt(b,324)+8);
            const int lengths=qFromLittleEndian<quint32>(b.constData()+tagAt(b,325)+8);
            const int offset=b.size(); b.append(payload);
            for(int i=0;i<72;++i) { put32(b,offsets+i*4,offset); put32(b,lengths+i*4,payload.size()); }
        }},
        {"oversized input", [](QByteArray& b) { b.resize(16*1024*1024+1); }}
    };
    if (OperaRadarImage::decode(valid).image.isNull()) { return 1; }
    for (const Mutation& mutation : corpus) {
        QByteArray bytes=valid; mutation.apply(bytes);
        const OperaRadarImage result=OperaRadarImage::decode(bytes);
        if (!result.image.isNull() || result.error.isEmpty()) {
            std::cerr << "Accepted malformed input: " << mutation.name << '\n'; return 2;
        }
    }
    std::cout << corpus.size() << " malformed-input cases rejected\n";
}
