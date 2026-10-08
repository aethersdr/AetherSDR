#pragma once

#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QCryptographicHash>

class QNetworkReply;

namespace AetherSDR {

// Fetches the pinned remote-access container image (TailnetShimRelease) and
// hands back a local path only if the bytes match the pinned size and
// SHA-256. A verified copy is cached and re-verified before reuse, so a
// second install needs no download and a tampered cache is never trusted.
class TailnetShimDownloader : public QObject {
    Q_OBJECT
public:
    explicit TailnetShimDownloader(QObject* parent = nullptr);

    // Pure check, separated for tests: empty when `path` holds exactly
    // `expectedSize` bytes hashing to `expectedSha256` (lower-case hex);
    // otherwise why not.
    static QString verifyFile(const QString& path, const QByteArray& expectedSha256,
                              qint64 expectedSize);

    // Where the verified image is cached.
    static QString cachePath();

    void start();
    void cancel();
    bool isRunning() const { return m_reply != nullptr; }

signals:
    void progress(qint64 received, qint64 total);
    void ready(const QString& path);       // verified image, safe to install
    void failed(const QString& message);

private:
    void onReadyRead();
    void onFinished();
    void fail(const QString& message);

    QNetworkAccessManager m_nam;
    QPointer<QNetworkReply> m_reply;
    QFile m_part;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    qint64 m_received{0};
};

}  // namespace AetherSDR
