#include "NetworkDiagnostics.h"

#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSet>
#include <QSslError>
#include <QSslSocket>
#include <QStringList>
#include <QUrl>
#include <QVariant>

Q_LOGGING_CATEGORY(lcNetwork, "aether.network", QtWarningMsg)

namespace AetherSDR::NetworkDiagnostics {

namespace {

constexpr char kSslErrorsProperty[] = "aetherSslErrors";

QMutex g_mutex;
QSet<QString> g_logged;
bool g_backendLogged = false;

QString backendLine()
{
    return QStringLiteral("TLS backend %1 (available: %2), supportsSsl=%3, library \"%4\"")
        .arg(QSslSocket::activeBackend(),
             QSslSocket::availableBackends().join(QLatin1Char(',')))
        .arg(QSslSocket::supportsSsl() ? QStringLiteral("yes") : QStringLiteral("no"))
        .arg(QSslSocket::sslLibraryVersionString());
}

QString origin(const QUrl& url)
{
    return url.host().isEmpty() ? QStringLiteral("<url>")
                                : QStringLiteral("%1://%2").arg(url.scheme(), url.host());
}

// Qt's error text quotes the request URL in full ("Error transferring <url> -
// server replied: ..."), and QRZ carries credentials in the query. Every URL
// in it becomes scheme://host: first the reply's own URLs in each spelling Qt
// may use (its default spelling decodes a %20 back to a space, which a pattern
// would stop at), then anything else that looks like a URL.
QString scrubUrls(QString text, const QNetworkReply* reply)
{
    QList<QUrl> urls{reply->url(), reply->request().url()};
    const QUrl redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    if (redirect.isValid()) {
        urls << reply->url().resolved(redirect);
    }
    for (const QUrl& url : urls) {
        if (!url.isValid()) {
            continue;
        }
        for (const QString& spelling : {url.toString(), url.toString(QUrl::FullyEncoded),
                                        url.toDisplayString(), url.toString(QUrl::RemoveUserInfo)}) {
            if (!spelling.isEmpty()) {
                text.replace(spelling, origin(url));
            }
        }
    }
    static const QRegularExpression anyUrl(QStringLiteral("[A-Za-z][A-Za-z0-9+.-]*://\\S+"));
    QString out;
    qsizetype last = 0;
    for (auto it = anyUrl.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        out += text.mid(last, m.capturedStart() - last);
        out += origin(QUrl(m.captured()));
        last = m.capturedEnd();
    }
    out += text.mid(last);
    return out;
}

} // namespace

QString safeErrorString(const QNetworkReply* reply)
{
    return scrubUrls(reply->errorString(), reply);
}

QString describeFailure(const QNetworkReply* reply, const char* what)
{
    const QUrl url = reply->url();
    QString line = QStringLiteral("%1: %2://%3 failed, error %4 (%5)")
        .arg(QLatin1String(what), url.scheme(), url.host())
        .arg(int(reply->error()))
        .arg(safeErrorString(reply));
    const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (status.isValid()) {
        line += QStringLiteral(", HTTP %1").arg(status.toInt());
    }
    const QStringList sslErrors = reply->property(kSslErrorsProperty).toStringList();
    if (!sslErrors.isEmpty()) {
        line += QStringLiteral(", TLS errors: ")
            + scrubUrls(sslErrors.join(QStringLiteral("; ")), reply);
    }
    return line;
}

void watch(QNetworkReply* reply, const char* what)
{
    if (!reply) {
        return;
    }
    QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                     [reply](const QList<QSslError>& errors) {
        QStringList list = reply->property(kSslErrorsProperty).toStringList();
        for (const QSslError& e : errors) {
            list << e.errorString();
        }
        reply->setProperty(kSslErrorsProperty, list);
    });
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, what]() {
        const QNetworkReply::NetworkError error = reply->error();
        if (error == QNetworkReply::NoError || error == QNetworkReply::OperationCanceledError) {
            return;
        }
        // The HTTP status is part of the key: a 429 and a later 410 from the
        // same host share one NetworkError and are different failures.
        const QString key = QStringLiteral("%1|%2|%3|%4")
            .arg(QLatin1String(what), reply->url().host())
            .arg(int(error))
            .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
        bool logBackend = false;
        {
            QMutexLocker lock(&g_mutex);
            if (g_logged.contains(key)) {
                return;
            }
            g_logged.insert(key);
            logBackend = !g_backendLogged;
            g_backendLogged = true;
        }
        if (logBackend) {
            qCWarning(lcNetwork).noquote() << backendLine();
        }
        qCWarning(lcNetwork).noquote() << describeFailure(reply, what);
    });
}

} // namespace AetherSDR::NetworkDiagnostics
