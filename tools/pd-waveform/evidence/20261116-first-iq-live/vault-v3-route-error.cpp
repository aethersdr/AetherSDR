#include "core/TailnetShimTokenStore.h"
#include <QCoreApplication>
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
 const QString path=QString::fromLocal8Bit(argv[1]);
 if(path!="status" && path!="aetherpd/installation" && path!="aetherpd/status" && path!="aetherpd/capture"
   && !QRegularExpression("^aetherpd/capture/[a-zA-Z0-9_-]{1,128}/[0-9]{1,2}$").match(path).hasMatch()){return 2;}
 QTimer::singleShot(20000,&app,[&]{app.exit(3);});
 TailnetShimTokenStore::load(QStringLiteral("2125-1213-8600-8895"),&app,[&](const QString&token){
  if(!QRegularExpression("^[0-9a-f]{64}$").match(token).hasMatch()){std::puts("credential_unavailable");app.exit(4);return;}
  QNetworkAccessManager network;network.setProxy(QNetworkProxy::NoProxy);
  QNetworkRequest request(QUrl("http://192.168.50.100:48992/"+QString(path.startsWith("aetherpd/capture")?"v3/":"v1/")+path));
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
  request.setRawHeader("Authorization","Bearer "+token.toLatin1());request.setTransferTimeout(12000);
  QNetworkReply*reply=path=="aetherpd/capture"?network.post(request,QByteArray{}):network.get(request);
  QEventLoop loop;QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);loop.exec();
  const int status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const QByteArray body=reply->read(65537);const QJsonDocument parsed=QJsonDocument::fromJson(body);
  const QJsonDocument result(QJsonObject{{"http_status",status},{"body",parsed.object()}});
  const QByteArray output=result.toJson(QJsonDocument::Compact);std::fwrite(output.constData(),1,output.size(),stdout);std::puts("");
  app.exit(status==200?0:1);
 });
 return app.exec();
}
