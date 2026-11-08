#pragma once

#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QCryptographicHash>

#include "TailnetShimRelease.h"

class QNetworkReply;

namespace AetherSDR {

// Fetches a pinned remote-access package (TailnetShimRelease for the
// container, TailnetShimNativeRelease for the legacy waveform) and hands
// back a local path only if the bytes match the pinned size and SHA-256. A verified copy is cached and re-verified before reuse, so a
// second install needs no download and a tampered cache is never trusted.
class TailnetShimDownloader : public QObject {
    Q_OBJECT
public:
    // What one package's release pins: AetherSDR accepts only these bytes.
    struct Pin {
        QString version;
        QString url;
        QByteArray sha256;   // lower-case hex
        qint64 size{0};
    };
    static Pin pin(TailnetShimPackage package);

    explicit TailnetShimDownloader(TailnetShimPackage package = TailnetShimPackage::Container,
                                   QObject* parent = nullptr);
    TailnetShimPackage package() const { return m_package; }

    // Pure check, separated for tests: empty when `path` holds exactly
    // `expectedSize` bytes hashing to `expectedSha256` (lower-case hex);
    // otherwise why not.
    static QString verifyFile(const QString& path, const QByteArray& expectedSha256,
                              qint64 expectedSize);

    // Moves a finished download (`part`) to `target` only if the bytes on
    // disk verify; otherwise deletes it. Empty on success, otherwise why not.
    static QString promoteVerified(const QString& part, const QString& target,
                                   const QByteArray& expectedSha256, qint64 expectedSize);

    // Where the verified package is cached. The native package keeps its
    // file name: the radio installs a legacy waveform by that name.
    static QString cachePath(TailnetShimPackage package = TailnetShimPackage::Container);

    // Deletes verified copies of earlier releases from `waveformsDir` (the
    // folder cachePath() uses): flex-tailnet-shim-*.tar.gz and
    // aether-tailnet-*/ other than the current pins. Returns what it removed.
    static QStringList removeStaleCopies(const QString& waveformsDir);

    void start();
    void cancel();
    bool isRunning() const { return m_reply != nullptr; }

signals:
    void progress(qint64 received, qint64 total);
    void ready(const QString& path);       // verified package, safe to install
    void failed(const QString& message);

private:
    void onReadyRead();
    void onFinished();
    void fail(const QString& message);

    const TailnetShimPackage m_package;
    const Pin m_pin;
    QNetworkAccessManager m_nam;
    QPointer<QNetworkReply> m_reply;
    QFile m_part;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    qint64 m_received{0};
};

}  // namespace AetherSDR
