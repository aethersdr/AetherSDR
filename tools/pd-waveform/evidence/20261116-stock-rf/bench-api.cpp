#include "core/TailnetShimTokenStore.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <cstdio>

int main(int argc, char** argv)
{
    using namespace AetherSDR;
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("AetherSDR"));
    if (argc < 2 || argc > 3) { return 2; }
    const QString command = QString::fromLocal8Bit(argv[1]);
    const QString id = argc == 3 ? QString::fromLocal8Bit(argv[2]) : QString{};
    const bool guardId = command == "guard-status" || command == "guard-stop";
    if (guardId ? !QRegularExpression("^[0-9a-f]{32}$").match(id).hasMatch()
                : argc != 2 || (command != "status" && command != "capture" && command != "guard-begin")) { return 2; }
    QTimer::singleShot(15000, &app, [&] { app.exit(3); });
    TailnetShimTokenStore::load(QStringLiteral("2125-1213-8600-8895"), &app, [&](const QString& token) {
        if (!QRegularExpression("^[0-9a-f]{64}$").match(token).hasMatch()) { app.exit(4); return; }
        QNetworkAccessManager network;
        network.setProxy(QNetworkProxy::NoProxy);
        bool valid = true;
        auto request = [&](const QString& path, bool post, const QByteArray& body = QByteArray{}) {
            QNetworkRequest req(QUrl("http://192.168.50.100:48992/" + path));
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
            req.setRawHeader("Authorization", "Bearer " + token.toLatin1());
            if (post) { req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json"); }
            req.setTransferTimeout(5000);
            QNetworkReply* reply = post ? network.post(req, body) : network.get(req);
            QEventLoop loop;
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            loop.exec();
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray bytes = reply->read(65537);
            QJsonParseError error;
            const QJsonDocument doc = QJsonDocument::fromJson(bytes, &error);
            if ((code != 200 && code != 202) || bytes.size() > 65536 || error.error != QJsonParseError::NoError || !doc.isObject()) { valid = false; }
            reply->deleteLater();
            return QJsonObject{{"http_status", code}, {"body", doc.object()}};
        };
        QJsonObject result;
        if (command == "status") { result = request("v1/aetherpd/status", false); }
        else if (command == "guard-begin") { result = request("v1/aetherpd/test-unkey", true, "{\"cleanup_after_ms\":9000}"); }
        else if (guardId) { result = request("v1/aetherpd/test-unkey/" + id + (command == "guard-stop" ? "/stop" : ""), command == "guard-stop"); }
        else {
            result = request("v3/aetherpd/capture", true);
            const QJsonObject manifest = result["body"].toObject();
            const QString captureId = manifest["capture_id"].toString();
            if (!QRegularExpression("^[0-9a-f]{32}$").match(captureId).hasMatch() || manifest["sample_count"].toInt() != 16384 || manifest["sample_rate_hz"].toInt() != 48000) { valid = false; }
            QByteArray raw;
            for (int i = 0; valid && i < 32; ++i) {
                const QJsonObject part = request("v1/aetherpd/capture/" + captureId + "/" + QString::number(i), false)["body"].toObject();
                const QByteArray encoded = part["iq_f32le"].toString().toLatin1();
                const QByteArray bytes = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
                if (part["capture_id"].toString() != captureId || part["offset"].toInt(-1) != i*512 || bytes.size() != 4096) { valid = false; break; }
                raw.append(bytes);
            }
            if (valid && raw.size() == 131072) {
                result["iq_f32le"] = QString::fromLatin1(raw.toBase64());
                result["iq_sha256"] = QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex());
            } else { valid = false; }
        }
        result["complete"] = valid;
        const QByteArray out = QJsonDocument(result).toJson(QJsonDocument::Compact);
        std::fwrite(out.constData(), 1, out.size(), stdout);
        std::puts("");
        app.exit(valid ? 0 : 1);
    });
    return app.exec();
}
