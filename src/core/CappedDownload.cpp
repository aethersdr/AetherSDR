#include "CappedDownload.h"

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <utility>

namespace AetherSDR {

CappedDownload::CappedDownload(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    // A stalled connection fails instead of hanging the install window.
    m_nam->setTransferTimeout(30000);
}

void CappedDownload::get(const QUrl& url, qint64 limit, const QString& tooLarge)
{
    cancel();
    m_limit = limit;
    m_tooLarge = tooLarge;
    m_body.clear();
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    m_reply = m_nam->get(request);
    m_reply->setReadBufferSize(64 * 1024);
    connect(m_reply, &QNetworkReply::metaDataChanged, this, [this] {
        bool known = false;
        const qint64 length =
            m_reply ? m_reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&known) : 0;
        if (known && length > m_limit)
            refuse();
    });
    connect(m_reply, &QNetworkReply::readyRead, this, &CappedDownload::readBody);
    connect(m_reply, &QNetworkReply::downloadProgress, this, &CappedDownload::progress);
    connect(m_reply, &QNetworkReply::finished, this, &CappedDownload::onFinished);
}

void CappedDownload::cancel()
{
    if (m_reply) {
        QNetworkReply* reply = std::exchange(m_reply, nullptr);
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
}

bool CappedDownload::readBody()
{
    // Never holds more than the limit: one byte past it is enough to refuse.
    if (m_reply)
        m_body += m_reply->read(m_limit + 1 - m_body.size());
    if (m_body.size() <= m_limit)
        return true;
    refuse();
    return false;
}

void CappedDownload::refuse()
{
    cancel();
    m_body.clear();
    emit failed(m_tooLarge);
}

void CappedDownload::onFinished()
{
    QNetworkReply* reply = m_reply;
    if (!reply)
        return;
    if (reply->error() == QNetworkReply::NoError && !readBody())
        return;   // refused: too large
    m_reply = nullptr;
    reply->deleteLater();
    const QUrl url = reply->url();
    if (reply->error() != QNetworkReply::NoError) {
        emit failed(QCoreApplication::translate("CappedDownload", "Could not download %1: %2")
                        .arg(url.fileName(), reply->errorString()));
        return;
    }
    emit finished(url, std::exchange(m_body, {}));
}

} // namespace AetherSDR
