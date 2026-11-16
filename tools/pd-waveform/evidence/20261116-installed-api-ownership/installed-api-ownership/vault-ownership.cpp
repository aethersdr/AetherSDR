#include "core/TailnetShimTokenStore.h"
#include <QCoreApplication>
#include <QFile>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkProxy>
#include <QRegularExpression>
#include <QTimer>
#include <cstdio>
using namespace AetherSDR;
int main(int argc,char**argv) {
 QCoreApplication app(argc,argv);app.setApplicationName(QStringLiteral("AetherSDR"));
 if(argc!=2){return 2;}
 const QString action=QString::fromLocal8Bit(argv[1]);
 if(action!="duplicate" && action!="malformed" && action!="foreign"){return 2;}
 QFile input;input.open(stdin,QIODevice::ReadOnly);const QByteArray data=input.read(513);
 const QJsonObject identity=QJsonDocument::fromJson(data).object();
 if(data.size()>512 || identity.size()!=4 || identity.value("gui_client_id").toString()!="5D14B8D4-A745-5620-BAF8-B3614CA1C814"
  || identity.value("gui_handle").toDouble()<=0 || identity.value("gui_handle").toDouble()>4294967295.0
  || identity.value("feedback_pan").toDouble()!=1073741824.0 || identity.value("iq_channel").toInt()!=4){return 2;}
 QJsonObject body=identity;
 if(action=="malformed"){body.insert("executable","/bin/false");}
 if(action=="foreign"){body.insert("gui_client_id","00000000-0000-0000-0000-000000000001");}
 const QByteArray payload=QJsonDocument(body).toJson(QJsonDocument::Compact);
 QTimer::singleShot(20000,&app,[&]{app.exit(3);});
 TailnetShimTokenStore::load(QStringLiteral("2125-1213-8600-8895"),&app,[&](const QString&token){
  if(!QRegularExpression("^[0-9a-f]{64}$").match(token).hasMatch()){std::puts("credential_unavailable");app.exit(4);return;}
  QNetworkAccessManager network;network.setProxy(QNetworkProxy::NoProxy);
  QNetworkRequest request(QUrl("http://192.168.50.100:48992/v2/aetherpd/start"));
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
  request.setRawHeader("Authorization","Bearer "+token.toLatin1());request.setTransferTimeout(12000);
  request.setHeader(QNetworkRequest::ContentTypeHeader,QStringLiteral("application/json"));
  QNetworkReply*reply=network.post(request,payload);
  QEventLoop loop;QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);loop.exec();
  const int status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const QByteArray body=reply->read(65537);const QJsonDocument parsed=QJsonDocument::fromJson(body);
  const QJsonDocument result(QJsonObject{{"http_status",status},{"body",parsed.object()}});
  const QByteArray output=result.toJson(QJsonDocument::Compact);std::fwrite(output.constData(),1,output.size(),stdout);std::puts("");
  app.exit(status>=100?0:1);
 });
 return app.exec();
}
