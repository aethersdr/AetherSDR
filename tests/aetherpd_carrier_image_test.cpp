#include "core/AetherPDCarrierImage.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QtEndian>
#include <zlib.h>

#include <iostream>

using namespace AetherSDR::aetherpd;
namespace {
bool check(bool value,const char* message)
{
    if(!value){std::cerr<<"FAIL: "<<message<<'\n';}return value;
}
QByteArray gunzip(const QByteArray& compressed)
{
    z_stream stream{};
    if(inflateInit2(&stream,MAX_WBITS+16)!=Z_OK){return {};}
    QByteArray bytes(24*1024*1024,'\0');
    stream.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(compressed.constData()));stream.avail_in=static_cast<uInt>(compressed.size());
    stream.next_out=reinterpret_cast<Bytef*>(bytes.data());stream.avail_out=static_cast<uInt>(bytes.size());
    const int code=inflate(&stream,Z_FINISH);const uLong size=stream.total_out;
    const bool complete=code==Z_STREAM_END&&stream.avail_in==0;inflateEnd(&stream);
    if(!complete){return {};}bytes.resize(static_cast<qsizetype>(size));return bytes;
}
struct Member{QByteArray data;unsigned mode;char type;};
QMap<QByteArray,Member> untar(const QByteArray& archive,bool& ok)
{
    QMap<QByteArray,Member> result;qsizetype offset=0;
    ok &= check(archive.size()%10240==0,"tar records are padded deterministically");
    while(offset+512<=archive.size()){
        const QByteArray header=archive.mid(offset,512);offset+=512;
        if(header==QByteArray(512,'\0')){
            ok &= check(archive.mid(offset)==QByteArray(archive.size()-offset,'\0'),"tar terminator padding");return result;
        }
        const QByteArray name=header.first(100).split('\0').first();
        bool valid=false;const qulonglong size=header.mid(124,12).split('\0').first().toULongLong(&valid,8);
        if(!valid||size>static_cast<qulonglong>(archive.size()-offset)){ok=false;return {};}
        const unsigned mode=header.mid(100,8).split('\0').first().toUInt(&valid,8);
        ok &= check(valid&&!result.contains(name),"unique valid tar member");
        ok &= check(header.mid(108,8)==QByteArray("0000000\0",8)&&header.mid(116,8)==QByteArray("0000000\0",8)
            &&header.mid(136,12)==QByteArray("00000000000\0",12),"fixed ownership and epoch");
        unsigned sum=0;
        for(int i=0;i<512;++i){sum += i>=148&&i<156 ? unsigned(' ') : static_cast<unsigned char>(header[i]);}
        ok &= check(sum==header.mid(148,6).toUInt(nullptr,8),"tar checksum");
        result.insert(name,{archive.mid(offset,static_cast<qsizetype>(size)),mode,header[156]});
        offset+=static_cast<qsizetype>((size+511)/512*512);
    }
    ok=false;return {};
}
QString sha(const QByteArray& bytes){return "sha256:"+QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
QByteArray blob(const QMap<QByteArray,Member>& members,const QJsonObject& descriptor,bool& ok)
{
    const QString digest=descriptor.value("digest").toString();
    const QByteArray data=members.value("blobs/sha256/"+digest.mid(7).toLatin1()).data;
    ok &= check(sha(data)==digest&&descriptor.value("size").toInteger()==data.size(),"OCI descriptor authenticates exact blob");return data;
}
QByteArray elf(char fill)
{
    QByteArray data(256,fill);data.replace(0,7,QByteArray("\x7f" "ELF\x02\x01\x01",7));qToLittleEndian<quint16>(183,data.data()+18);return data;
}
QByteArray receipt()
{
    QByteArray data("PDPUB001PDDELIV1");data.resize(24);qToBigEndian<quint32>(64,data.data()+16);qToBigEndian<quint32>(2,data.data()+20);
    return data+QByteArray(32,'e')+QByteArray(32,'p')+QByteArray(32,'c');
}
}
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);bool ok=true;
    const CarrierExecutables code{elf('a'),elf('c'),elf('s'),elf('d')};
    const QByteArray publicRecord=receipt(),licenses="Public test fixtures; no private credentials.\n";
    const std::optional<CarrierImage> image=buildCarrierImage(code,publicRecord,licenses);
    if(!check(image.has_value(),"build public image")){return 1;}
    const std::optional<CarrierImage> repeat=buildCarrierImage(code,publicRecord,licenses);
    ok &= check(repeat&&repeat->archive==image->archive,"same inputs produce identical image bytes");
    ok &= check(image->containerName=="aetherpd-carrier-63636363636363636363636363636363"
        &&image->archiveSha256==sha(image->archive).mid(7),"request-specific name and outer hash");
    ok &= check(image->archive.mid(4,4)==QByteArray(4,'\0')&&static_cast<unsigned char>(image->archive[9])==255,"gzip epoch and OS independent header");
    const QMap<QByteArray,Member> outer=untar(gunzip(image->archive),ok);
    ok &= check(outer.size()==7&&outer.value("oci-layout").data=="{\"imageLayoutVersion\":\"1.0.0\"}","fixed OCI archive entries");
    const QJsonObject index=QJsonDocument::fromJson(outer.value("index.json").data).object();
    const QJsonObject descriptor=index.value("manifests").toArray().first().toObject();
    const QJsonObject manifest=QJsonDocument::fromJson(blob(outer,descriptor,ok)).object();
    const QByteArray configBytes=blob(outer,manifest.value("config").toObject(),ok);
    const QJsonObject config=QJsonDocument::fromJson(configBytes).object();
    ok &= check(image->imageId==sha(configBytes).mid(7),"reported image ID is config digest");
    const QJsonObject run=config.value("config").toObject();
    ok &= check(run.value("Entrypoint").toArray()==QJsonArray{"/pd/accept"}&&run.value("User")=="0:0"&&run.size()==3,"fixed executable entry, UID, and labels only");
    ok &= check(run.value("Labels").toObject().value("com.flexradio.waveform.name")==image->containerName,"radio removal name matches image label");
    const QByteArray layerTar=gunzip(blob(outer,manifest.value("layers").toArray().first().toObject(),ok));
    ok &= check(config.value("rootfs").toObject().value("diff_ids").toArray().first()==sha(layerTar)&&image->layerBytes==layerTar.size(),"uncompressed layer digest and accounting");
    const QMap<QByteArray,Member> layer=untar(layerTar,ok);
    ok &= check(layer.size()==7&&layer.value("pd/request").data==publicRecord&&layer.value("pd/request").mode==0444
        &&layer.value("LICENSES").data==licenses&&layer.value("pd/").type=='5',"fixed public request and license layout");
    ok &= check(layer.value("pd/accept").data==code.accept&&layer.value("pd/carrier").data==code.carrier
        &&layer.value("pd/send").data==code.send&&layer.value("pd/deliver").data==code.deliver,"executables unmodified");
    for(const QByteArray& name : {QByteArray("pd/accept"),QByteArray("pd/carrier"),QByteArray("pd/send"),QByteArray("pd/deliver")}){
        ok &= check(layer.value(name).mode==0500,"executable mode is fixed");
    }
    for(const int offset:{0,8,16,20}){
        QByteArray bad=publicRecord;bad[offset]^=1;ok &= check(!buildCarrierImage(code,bad,licenses),"invalid receipt refused");
    }
    for(const int offset:{24,56,88}){
        QByteArray bad=publicRecord;bad.replace(offset,32,QByteArray(32,'\0'));ok &= check(!buildCarrierImage(code,bad,licenses),"zero binding refused");
    }
    ok &= check(!buildCarrierImage(code,"PDXFER01"+QByteArray(112,'x'),licenses),"private configuration cannot enter archive");
    CarrierExecutables bad=code;bad.accept[18]=0;ok &= check(!buildCarrierImage(bad,publicRecord,licenses),"wrong architecture refused");
    bad=code;bad.send=QByteArray(8*1024*1024+1,'x');ok &= check(!buildCarrierImage(bad,publicRecord,licenses),"executable size bounded");
    ok &= check(!buildCarrierImage(code,publicRecord,{})&&!buildCarrierImage(code,publicRecord,QByteArray(512*1024+1,'x')),"license input required and bounded");
    QByteArray other=publicRecord;other[88]^=1;
    const std::optional<CarrierImage> unique=buildCarrierImage(code,other,licenses);
    ok &= check(unique&&unique->containerName!=image->containerName&&unique->imageId!=image->imageId,"different requests have distinct image and cleanup names");
    if(argc==2){QFile output(QString::fromLocal8Bit(argv[1]));if(!output.open(QIODevice::WriteOnly)||output.write(image->archive)!=image->archive.size()){return 2;}}
    return ok?0:1;
}
