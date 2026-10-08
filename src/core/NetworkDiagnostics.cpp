#include "NetworkDiagnostics.h"

#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkReply>
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

} // namespace

QString describeFailure(const QNetworkReply* reply, const char* what)
{
    const QUrl url = reply->url();
    QString line = QStringLiteral("%1: %2://%3 failed, error %4 (%5)")
        .arg(QLatin1String(what), url.scheme(), url.host())
        .arg(int(reply->error()))
        .arg(reply->errorString());
    const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (status.isValid()) {
        line += QStringLiteral(", HTTP %1").arg(status.toInt());
    }
    const QStringList sslErrors = reply->property(kSslErrorsProperty).toStringList();
    if (!sslErrors.isEmpty()) {
        line += QStringLiteral(", TLS errors: ") + sslErrors.join(QStringLiteral("; "));
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
        const QString key = QStringLiteral("%1|%2|%3")
            .arg(QLatin1String(what), reply->url().host()).arg(int(error));
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
