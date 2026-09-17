#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QVector>
#include <memory>

class QCryptographicHash;
class QLockFile;
class QNetworkAccessManager;
class QNetworkReply;
class QSaveFile;

namespace AetherSDR {

// First-use bundle preparation. Follows the ASR download/cache pattern, with
// bounded writes and cancellation covering cache verification as well as I/O.
class DeepFistModelAssets final : public QObject {
    Q_OBJECT
public:
    struct Asset { QString name; qint64 bytes; QByteArray sha256; };
    static QVector<Asset> manifest();
    static QString releaseBaseUrl();
    // Network injection and an explicit catalog make socket-free tests possible.
    explicit DeepFistModelAssets(QString directory, QString baseUrl = releaseBaseUrl(),
        QVector<Asset> assets = manifest(), QNetworkAccessManager* network = nullptr,
        QObject* parent = nullptr);
    ~DeepFistModelAssets() override;
    void ensure();
    void cancel();
    bool busy() const { return m_busy; }
signals:
    void checking();
    void progress(qint64 received, qint64 total);
    void ready();
    void failed(const QString& reason);
private:
    void next();
    void download();
    void readAvailable();
    void finishDownload();
    void fail(const QString& reason);
    void cleanup();
    QString m_directory;
    QString m_baseUrl;
    QVector<Asset> m_assets;
    QNetworkAccessManager* m_network;
    QNetworkReply* m_reply = nullptr;
    std::unique_ptr<QSaveFile> m_file;
    std::unique_ptr<QCryptographicHash> m_hash;
    std::unique_ptr<QLockFile> m_lock;
    quint64 m_generation = 0;
    qsizetype m_index = 0;
    qint64 m_received = 0;
    qint64 m_completed = 0;
    qint64 m_total = 0;
    bool m_busy = false;
};
}
