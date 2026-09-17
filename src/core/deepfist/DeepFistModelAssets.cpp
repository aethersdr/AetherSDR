#include "DeepFistModelAssets.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent>

namespace AetherSDR {
namespace {
bool matches(const QString& path, const DeepFistModelAssets::Asset& asset)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() != asset.bytes) { return false; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) && hash.result().toHex() == asset.sha256;
}
}
QVector<DeepFistModelAssets::Asset> DeepFistModelAssets::manifest()
{
    return {
        {"deepfist.onnx", 13051998, "6d2d4e3d66f9001d15e21a1b38b79150eae19ead86a310202900ee69d672b94d"},
        {"deepfist.onnx.json", 1257, "840ceb8dba9d46d04495547a8a3789968b1acd2f8ac3a3a5c631f84008ac2217"},
        {"LICENSE", 1068, "9ad70a9ed30d58502e29f9e691a008ee7bccb6eba49d4384f2b7e675d68dc4f3"}
    };
}
QString DeepFistModelAssets::releaseBaseUrl()
{
    // Empty until a reviewed, versioned release asset set has been published.
    return QString::fromUtf8(DEEPFIST_MODEL_BASE_URL);
}
DeepFistModelAssets::DeepFistModelAssets(QString directory, QString baseUrl,
    QVector<Asset> assets, QNetworkAccessManager* network, QObject* parent)
    : QObject(parent), m_directory(std::move(directory)), m_baseUrl(std::move(baseUrl)),
      m_assets(std::move(assets)), m_network(network)
{
    if (!m_network) { m_network = new QNetworkAccessManager(this); }
}
DeepFistModelAssets::~DeepFistModelAssets() { cancel(); }
void DeepFistModelAssets::cleanup()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_file.reset(); // QSaveFile removes an uncommitted temporary file.
    m_hash.reset();
    m_lock.reset();
}
void DeepFistModelAssets::cancel()
{
    ++m_generation;
    m_busy = false;
    cleanup();
}
void DeepFistModelAssets::fail(const QString& reason)
{
    cancel();
    emit failed(reason);
}
void DeepFistModelAssets::ensure()
{
    if (m_busy) { return; }
    m_busy = true;
    ++m_generation;
    m_index = 0;
    m_completed = 0;
    m_total = 0;
    for (const Asset& asset : m_assets) {
        if (asset.name.isEmpty() || asset.name == "." || asset.name == ".."
            || asset.name.contains('/') || asset.name.contains('\\') || asset.bytes <= 0
            || asset.sha256.size() != 64) {
            fail(tr("Invalid model manifest"));
            return;
        }
        m_total += asset.bytes;
    }
    if (m_assets.isEmpty() || !QDir().mkpath(m_directory)) {
        fail(tr("Cannot create the model cache"));
        return;
    }
    m_lock = std::make_unique<QLockFile>(QDir(m_directory).filePath(".download.lock"));
    if (!m_lock->tryLock(0)) {
        fail(tr("Another app is preparing this model. Retry when it finishes."));
        return;
    }
    next();
}
void DeepFistModelAssets::next()
{
    if (!m_busy) { return; }
    if (m_index == m_assets.size()) {
        m_busy = false;
        cleanup();
        emit ready();
        return;
    }
    const quint64 generation = m_generation;
    const Asset asset = m_assets[m_index];
    const QString path = QDir(m_directory).filePath(asset.name);
    auto* watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher, generation] {
        const bool valid = watcher->result();
        watcher->deleteLater();
        if (!m_busy || generation != m_generation) { return; }
        if (valid) {
            m_completed += m_assets[m_index++].bytes;
            next();
        } else {
            download();
        }
    });
    // The task owns its inputs and never captures this; destruction is safe
    // while hashing, and a canceled generation cannot begin another download.
    watcher->setFuture(QtConcurrent::run([path, asset] { return matches(path, asset); }));
    emit checking();
}
void DeepFistModelAssets::download()
{
    const QUrl base(m_baseUrl);
    if (base.scheme() != "https" || base.host().isEmpty() || base.hasQuery() || base.hasFragment()) {
        fail(tr("Model unavailable: the download release has not been configured."));
        return;
    }
    const Asset& asset = m_assets[m_index];
    m_received = 0;
    m_file = std::make_unique<QSaveFile>(QDir(m_directory).filePath(asset.name));
    m_file->setDirectWriteFallback(false);
    if (!m_file->open(QIODevice::WriteOnly)) { fail(tr("Cannot write the model cache")); return; }
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    const QString prefix = m_baseUrl.endsWith('/') ? m_baseUrl : m_baseUrl + '/';
    QNetworkRequest request(QUrl(prefix + asset.name));
    request.setTransferTimeout(30000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("AetherSDR-DeepFist"));
    m_reply = m_network->get(request);
    m_reply->setReadBufferSize(64 * 1024);
    connect(m_reply, &QNetworkReply::readyRead, this, &DeepFistModelAssets::readAvailable);
    connect(m_reply, &QNetworkReply::finished, this, &DeepFistModelAssets::finishDownload);
    emit progress(m_completed, m_total);
}
void DeepFistModelAssets::readAvailable()
{
    if (!m_reply || !m_file) { return; }
    const QPointer<DeepFistModelAssets> guard(this);
    const quint64 generation = m_generation;
    while (m_reply && m_reply->bytesAvailable() > 0) {
        const QByteArray bytes = m_reply->read(64 * 1024);
        if (bytes.isEmpty()) { break; }
        if (bytes.size() > m_assets[m_index].bytes - m_received) {
            fail(tr("Downloaded model exceeds its expected size")); return;
        }
        if (m_file->write(bytes) != bytes.size()) { fail(tr("Cannot save the downloaded model")); return; }
        m_hash->addData(bytes);
        m_received += bytes.size();
        emit progress(m_completed + m_received, m_total);
        // A status observer can replace the backend and destroy this manager.
        if (!guard || !m_busy || generation != m_generation) { return; }
    }
}
void DeepFistModelAssets::finishDownload()
{
    if (!m_reply) { return; }
    const QPointer<DeepFistModelAssets> guard(this);
    const quint64 generation = m_generation;
    if (m_reply->error() != QNetworkReply::NoError
        || m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
        fail(tr("Model download failed. Check your connection and retry.")); return;
    }
    readAvailable();
    if (!guard || !m_reply || generation != m_generation) { return; }
    const Asset& asset = m_assets[m_index];
    if (m_received != asset.bytes || m_hash->result().toHex() != asset.sha256) {
        fail(tr("Model verification failed. Retry the download.")); return;
    }
    if (!m_file->commit()) { fail(tr("Cannot install the verified model")); return; }
    m_file.reset();
    m_hash.reset();
    m_reply->disconnect(this);
    m_reply->deleteLater();
    m_reply = nullptr;
    m_completed += asset.bytes;
    ++m_index;
    // Avoid recursive signal delivery across asset completions.
    QTimer::singleShot(0, this, [this, generation] {
        if (m_busy && generation == m_generation) { next(); }
    });
}
}
