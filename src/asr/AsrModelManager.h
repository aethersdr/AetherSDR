#pragma once

#include "asr/AsrModelCatalog.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;

namespace AetherSDR {

// Downloads and caches Whisper (ggml) model weights for the on-device ASR
// engine (RFC #4333). Weights are never shipped: on first enable this fetches
// the selected tier, trying each catalog source in order (Hugging Face primary,
// GitHub release-asset fallback) and accepting a file only if its SHA-256
// matches the pinned value — so a corrupt download or a diverged mirror is
// rejected and the next source is tried. Downloads stream to a `.part` file and
// are renamed into place atomically only after verification.
//
// Engine-only class (Qt Core/Network); no GUI dependency. One download at a
// time per instance. Cancel aborts and removes the partial file.
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

    bool isBusy() const { return m_reply != nullptr || m_verifying; }

public slots:
    // Ensure the tier is cached and valid. Emits alreadyPresent() immediately
    // if so; otherwise downloads with source failover + verification. A second
    // call while busy fails fast.
    void ensure(const AsrModelTier& tier);

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
};

} // namespace AetherSDR
