#include "TailnetShimDownloader.h"

#include "TailnetShimRelease.h"

#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

namespace AetherSDR {

namespace {
// About 8 MB; a slow link still finishes, a stalled one fails visibly.
constexpr int kTransferTimeoutMs = 120'000;
}  // namespace

TailnetShimDownloader::TailnetShimDownloader(QObject* parent)
    : QObject(parent)
{
    m_nam.setTransferTimeout(kTransferTimeoutMs);
    // GitHub release assets redirect to a CDN; never follow to plain HTTP.
    m_nam.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

QString TailnetShimDownloader::verifyFile(const QString& path, const QByteArray& expectedSha256,
                                          qint64 expectedSize)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return tr("cannot read %1").arg(QDir::toNativeSeparators(path));
    }
    if (f.size() != expectedSize) {
        return tr("size is %1 bytes, expected %2").arg(f.size()).arg(expectedSize);
    }
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f)) {
        return tr("cannot read %1").arg(QDir::toNativeSeparators(path));
    }
    if (h.result().toHex() != expectedSha256.toLower()) {
        return tr("SHA-256 does not match the pinned release");
    }
    return {};
}

QString TailnetShimDownloader::cachePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/waveforms/flex-tailnet-shim-%1.tar.gz")
              .arg(QString::fromLatin1(TailnetShimRelease::kVersion));
}

void TailnetShimDownloader::start()
{
    if (m_reply) {
        return;
    }
    const QByteArray sha = QByteArray(TailnetShimRelease::kSha256);
    const QString target = cachePath();
    if (QFileInfo::exists(target)) {
        if (verifyFile(target, sha, TailnetShimRelease::kSize).isEmpty()) {
            QTimer::singleShot(0, this, [this, target] { emit ready(target); });
            return;
        }
        QFile::remove(target);   // a damaged or tampered cache is never trusted
    }
    QDir().mkpath(QFileInfo(target).absolutePath());
    m_part.setFileName(target + QStringLiteral(".part"));
    if (!m_part.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(tr("Cannot write the download to %1.")
                 .arg(QDir::toNativeSeparators(m_part.fileName())));
        return;
    }
    m_hash.reset();
    m_received = 0;
    m_reply = m_nam.get(QNetworkRequest(QUrl(QString::fromLatin1(TailnetShimRelease::kUrl))));
    connect(m_reply, &QNetworkReply::readyRead, this, &TailnetShimDownloader::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &TailnetShimDownloader::onFinished);
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64) {
        emit progress(got, TailnetShimRelease::kSize);
    });
}

void TailnetShimDownloader::onReadyRead()
{
    if (!m_reply) {
        return;
    }
    const QByteArray chunk = m_reply->readAll();
    m_received += chunk.size();
    if (m_received > TailnetShimRelease::kSize) {
        // Never buffer more than the pinned image: stop as soon as it overruns.
        fail(tr("The download is larger than the pinned release; it was discarded."));
        return;
    }
    m_hash.addData(chunk);
    m_part.write(chunk);
}

void TailnetShimDownloader::onFinished()
{
    if (!m_reply) {
        return;
    }
    if (m_reply->error() == QNetworkReply::NoError) {
        onReadyRead();   // drain the last chunk while the reply is still ours
        if (!m_reply) {
            return;      // the drain overran the pinned size; fail() reported it
        }
    }
    QNetworkReply* reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        m_part.close();
        m_part.remove();
        if (reply->error() == QNetworkReply::OperationCanceledError) {
            emit failed(tr("Download cancelled."));
        } else {
            emit failed(tr("Download failed: %1").arg(reply->errorString()));
        }
        return;
    }
    m_part.close();
    const QString part = m_part.fileName();
    if (m_received != TailnetShimRelease::kSize
        || m_hash.result().toHex() != QByteArray(TailnetShimRelease::kSha256)) {
        QFile::remove(part);
        emit failed(tr("The downloaded image does not match the release AetherSDR expects "
                       "(size or SHA-256), so it was discarded and nothing was installed."));
        return;
    }
    const QString target = cachePath();
    QFile::remove(target);
    if (!QFile::rename(part, target)) {
        QFile::remove(part);
        emit failed(tr("Cannot save the verified image to %1.")
                        .arg(QDir::toNativeSeparators(target)));
        return;
    }
    emit ready(target);
}

void TailnetShimDownloader::cancel()
{
    if (m_reply) {
        m_reply->abort();
    }
}

void TailnetShimDownloader::fail(const QString& message)
{
    if (m_reply) {
        QNetworkReply* reply = m_reply;
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    if (m_part.isOpen()) {
        m_part.close();
    }
    m_part.remove();
    emit failed(message);
}

}  // namespace AetherSDR
