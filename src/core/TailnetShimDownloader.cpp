#include "TailnetShimDownloader.h"

#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

namespace AetherSDR {

namespace {
// About 9 MB; a slow link still finishes, a stalled one fails visibly.
constexpr int kTransferTimeoutMs = 120'000;
}  // namespace

TailnetShimDownloader::Pin TailnetShimDownloader::pin(TailnetShimPackage package)
{
    if (package == TailnetShimPackage::Native) {
        return {QString::fromLatin1(TailnetShimNativeRelease::kVersion),
                QString::fromLatin1(TailnetShimNativeRelease::kUrl),
                QByteArray(TailnetShimNativeRelease::kSha256),
                TailnetShimNativeRelease::kSize};
    }
    return {QString::fromLatin1(TailnetShimRelease::kVersion),
            QString::fromLatin1(TailnetShimRelease::kUrl),
            QByteArray(TailnetShimRelease::kSha256),
            TailnetShimRelease::kSize};
}

TailnetShimDownloader::TailnetShimDownloader(TailnetShimPackage package, QObject* parent)
    : QObject(parent)
    , m_package(package)
    , m_pin(pin(package))
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

QString TailnetShimDownloader::cachePath(TailnetShimPackage package)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/waveforms/");
    if (package == TailnetShimPackage::Native) {
        return dir + QStringLiteral("aether-tailnet-%1/AetherTailnet.ssdr_waveform")
                         .arg(pin(package).version);
    }
    return dir + QStringLiteral("flex-tailnet-shim-%1.tar.gz").arg(pin(package).version);
}

void TailnetShimDownloader::start()
{
    if (m_reply) {
        return;
    }
    const QString target = cachePath(m_package);
    if (QFileInfo::exists(target)) {
        if (verifyFile(target, m_pin.sha256, m_pin.size).isEmpty()) {
            // Both packages cache under the same waveforms/ folder.
            removeStaleCopies(QFileInfo(cachePath()).absolutePath());
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
    m_reply = m_nam.get(QNetworkRequest(QUrl(m_pin.url)));
    connect(m_reply, &QNetworkReply::readyRead, this, &TailnetShimDownloader::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &TailnetShimDownloader::onFinished);
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64) {
        emit progress(got, m_pin.size);
    });
}

void TailnetShimDownloader::onReadyRead()
{
    if (!m_reply) {
        return;
    }
    const QByteArray chunk = m_reply->readAll();
    m_received += chunk.size();
    if (m_received > m_pin.size) {
        // Never buffer more than the pinned package: stop as soon as it overruns.
        fail(tr("The download is larger than the pinned release; it was discarded."));
        return;
    }
    m_hash.addData(chunk);
    if (m_part.write(chunk) != chunk.size()) {
        fail(tr("Cannot write the download: %1.").arg(m_part.errorString()));
    }
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
    const bool flushed = m_part.flush();
    const QString flushError = m_part.errorString();
    m_part.close();
    const QString part = m_part.fileName();
    if (!flushed) {
        QFile::remove(part);
        emit failed(tr("Cannot write the download: %1.").arg(flushError));
        return;
    }
    if (m_received != m_pin.size || m_hash.result().toHex() != m_pin.sha256) {
        QFile::remove(part);
        emit failed(tr("The download does not match the release AetherSDR expects "
                       "(size or SHA-256), so it was discarded and nothing was installed."));
        return;
    }
    const QString target = cachePath(m_package);
    const QString error = promoteVerified(part, target, m_pin.sha256, m_pin.size);
    if (!error.isEmpty()) {
        emit failed(error);
        return;
    }
    removeStaleCopies(QFileInfo(cachePath()).absolutePath());
    emit ready(target);
}

QString TailnetShimDownloader::promoteVerified(const QString& part, const QString& target,
                                               const QByteArray& expectedSha256,
                                               qint64 expectedSize)
{
    // The network bytes were hashed as they arrived; this checks the bytes
    // that actually reached the disk, which a full disk or an I/O error can
    // make differ.
    const QString why = verifyFile(part, expectedSha256, expectedSize);
    if (!why.isEmpty()) {
        QFile::remove(part);
        return tr("The saved file does not match the release AetherSDR expects (%1), so it "
                  "was discarded and nothing was installed.").arg(why);
    }
    QFile::remove(target);
    if (!QFile::rename(part, target)) {
        QFile::remove(part);
        return tr("Cannot save the verified package to %1.").arg(QDir::toNativeSeparators(target));
    }
    return {};
}

QStringList TailnetShimDownloader::removeStaleCopies(const QString& waveformsDir)
{
    const QString container = QFileInfo(cachePath(TailnetShimPackage::Container)).fileName();
    const QString native =
        QFileInfo(QFileInfo(cachePath(TailnetShimPackage::Native)).absolutePath()).fileName();
    QStringList removed;
    const QDir dir(waveformsDir);
    for (const QFileInfo& f : dir.entryInfoList({QStringLiteral("flex-tailnet-shim-*.tar.gz")},
                                                QDir::Files)) {
        if (f.fileName() != container && QFile::remove(f.absoluteFilePath())) {
            removed << f.fileName();
        }
    }
    for (const QFileInfo& d : dir.entryInfoList({QStringLiteral("aether-tailnet-*")},
                                                QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (d.fileName() != native && !d.isSymLink()
            && QDir(d.absoluteFilePath()).removeRecursively()) {
            removed << d.fileName();
        }
    }
    return removed;
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
