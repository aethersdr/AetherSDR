#pragma once

#include "asr/AsrModelCatalog.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;

namespace AetherSDR {

// Downloads and caches ASR model files (RFC #4333): Whisper (ggml) weights, or
// the several files of a sherpa-onnx bundle. Tries each catalog source in order
// and accepts a file only if its SHA-256 matches the pinned value; streams to
// `.part` and renames atomically after verification.
// Qt Core/Network only. One download at a time; cancel removes the partial file.
class AsrModelManager : public QObject {
    Q_OBJECT

public:
    // Production: cache under defaultModelsDir() (AppDataLocation/models, de-nested).
    explicit AsrModelManager(QObject* parent = nullptr);

    // Dependency-injection ctor for tests: explicit cache directory and an
    // optional network manager. If nam is null one is created and owned;
    // otherwise the caller retains ownership.
    AsrModelManager(QString cacheDir, QNetworkAccessManager* nam,
                    QObject* parent = nullptr);

    ~AsrModelManager() override;

    // The default models cache directory (AppDataLocation/models), with the macOS
    // <org>/<app> double-nest collapsed. Shared with the UI's file-picker defaults
    // so downloads and manual picks point at the same place.
    static QString defaultModelsDir();

    QString cacheDir() const { return m_cacheDir; }
    QString modelPath(const AsrModelTier& tier) const;

    // Cheap check: file exists and its size equals the pinned size. Does not
    // hash — use verify() for full integrity.
    bool isPresent(const AsrModelTier& tier) const;

    // Full SHA-256 verification of the cached file. Returns false (and sets
    // *error if non-null) when the file is missing, mis-sized, or mismatched.
    bool verify(const AsrModelTier& tier, QString* error = nullptr) const;

    // Cheap check for a multi-file model: every file exists at its pinned size.
    // Does not hash.
    bool isBundlePresent(const QVector<AsrModelTier>& files) const;

    // Absolute folder holding a bundle's files (that of the first file).
    QString bundlePath(const QVector<AsrModelTier>& files) const;

    bool isBusy() const { return m_reply != nullptr || m_verifying || !m_bundle.isEmpty(); }

public slots:
    // Ensure the tier is cached and valid. Emits alreadyPresent() immediately
    // if so; otherwise downloads with source failover + verification. A second
    // call while busy fails fast.
    void ensure(const AsrModelTier& tier);

    // Ensure every file of a multi-file model is cached and valid, one after another,
    // with the same per-file source failover and SHA-256 check as ensure(). All files
    // must sit in the same folder under the cache (fileName "<folder>/<file>").
    // Signals report the bundle, not the file: progress() counts bytes across the
    // whole bundle (files already cached count as received) and is emitted only
    // once something is being downloaded, verifying() as for a file, and on success alreadyPresent(folder) if nothing had to be fetched or
    // finished(folder) if anything was, where folder is the absolute bundle path.
    // failed() stops the bundle at the first file that cannot be fetched; files
    // already verified stay cached. Fails fast if busy, like ensure().
    void ensureBundle(const QVector<AsrModelTier>& files);

    // Abort an in-flight download and delete the partial file.
    void cancel();

signals:
    void progress(qint64 received, qint64 total); // total == -1 when unknown
    void verifying();                              // SHA-256 check started (background)
    void alreadyPresent(const QString& modelPath);
    void finished(const QString& modelPath);
    void failed(const QString& error);

private:
    QString partPath(const AsrModelTier& tier) const;
    bool verifyFile(const QString& path, const AsrModelTier& tier, QString* error) const;
    // Hash+verify `path` on a background thread (never the UI thread — a multi-GB
    // model would freeze the app), then run `onDone(match)` back on this thread.
    void verifyInBackground(const QString& path, std::function<void(bool)> onDone);
    // ensure() without the busy fail-fast, so a bundle can chain its files.
    void ensureFile(const AsrModelTier& tier);
    // A file is cached and valid (`fetched` = it was downloaded just now). Single
    // mode: emits alreadyPresent/finished. Bundle mode: moves on to the next file,
    // and after the last emits the bundle's terminal signal.
    void completeFile(const QString& path, bool fetched);
    // Terminal failure: drops any bundle state, then emits failed().
    void fail(const QString& error);
    void beginDownload();
    void startSource(int index);
    void onReadyRead();
    void onReplyFinished();
    void cleanupReply();
    void abandonPartFile();

    QString m_cacheDir;
    QNetworkAccessManager* m_nam = nullptr;
    bool m_ownsNam = false;

    // In-flight download state.
    AsrModelTier m_tier;
    int m_sourceIndex = -1;
    QNetworkReply* m_reply = nullptr;
    std::unique_ptr<QFile> m_partFile;
    QStringList m_sourceErrors; // one line per failed source, for the final message
    bool m_canceled = false;
    bool m_verifying = false;   // a background verification is running

    // Bundle state; m_bundle is empty outside ensureBundle().
    QVector<AsrModelTier> m_bundle;
    int m_bundleIndex = 0;
    qint64 m_bundleTotalBytes = 0;
    qint64 m_bundleDoneBytes = 0;   // bytes of the files already completed
    bool m_bundleFetched = false;   // any file needed a download
};

} // namespace AetherSDR
