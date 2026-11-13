#pragma once

#include <QByteArray>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;

namespace AetherSDR {

// One download of at most a given size, for firmware and its lists: more is
// refused as it arrives (and at once when the server announces more), before
// it is buffered. Http(s) and file URLs alike; a redirect may not lower the
// security of the URL, and a stalled connection fails after 30 s. A
// connection-level failure before the first body byte is retried once, over
// HTTP/1.1. One download at a time; get() cancels one in progress.
// True for a failure that refused the request before any of its body was
// sent: a connection reset, an HTTP/2 GOAWAY (ContentReSendError, or
// ProtocolUnknownError when it carries an error code), a stream reset, or an
// HTTP/2 frame that could not be sent. HTTP status, timeout, cancel, redirect
// and TLS failures are final.
bool isRetryableBeforeBody(QNetworkReply::NetworkError error, bool http2Used);

class CappedDownload : public QObject {
    Q_OBJECT

public:
    explicit CappedDownload(QObject* parent = nullptr);

    // tooLarge: the message failed() carries when the body passes limit.
    void get(const QUrl& url, qint64 limit, const QString& tooLarge);
    // Drops the download in progress; nothing is emitted for it.
    void cancel();
    bool isBusy() const { return m_reply != nullptr; }

signals:
    void progress(qint64 done, qint64 total);
    void finished(const QUrl& url, const QByteArray& body);
    void failed(const QString& message);

private:
    bool readBody();
    void refuse();
    void onFinished();
    void startRequest();

    QNetworkAccessManager* m_nam;
    QNetworkReply* m_reply{nullptr};
    QUrl m_url;
    qint64 m_limit{0};
    bool m_retried{false};
    QString m_tooLarge;
    QByteArray m_body;
};

} // namespace AetherSDR
