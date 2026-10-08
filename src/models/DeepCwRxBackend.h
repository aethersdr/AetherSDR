#pragma once
#include "CwRxModel.h"
#include <QMutex>
#include <QString>
#include <atomic>
#include <memory>
#include <vector>

class QNetworkAccessManager;
class QThread;

namespace AetherSDR {

class DeepCwEngine;
class DeepFistModelAssets;

// DeepCW (RFC #4817, K5PTB's port of e04/deepcw-engine) behind CwRxBackend.
// Consumes the converted mono 24 kHz feed (feedFixed24), resamples it to the
// model's 3200 Hz on its own worker, and commits text through DeepCwCommitter.
// start() first prepares the model: cached file verified, else downloaded from
// e04's repository at a pinned commit (upstream source only, no mirror).
class DeepCwRxBackend final : public CwRxBackend {
    Q_OBJECT
public:
    explicit DeepCwRxBackend(QObject* parent = nullptr);
    // Directory, source and network are injectable so tests need no real cache.
    DeepCwRxBackend(QString directory, QString baseUrl, QNetworkAccessManager* network,
                    QObject* parent = nullptr);
    ~DeepCwRxBackend() override;

    static QString modelDirectory();
    static QString modelBaseUrl();

    void start() override;
    void stop() override;
    void reset() override;
    void feedFixed24(const DecoderPcmBlock& block) override;
    bool isRunning() const override { return m_started; }
    float estimatedPitch() const override { return m_pitch.load(); }
    QString status() const override { return m_status; }
    QString detail() const override { return m_detail; }
    bool preparing() const override { return m_preparing; }
    bool canRetry() const override { return m_canRetry; }
    void cancelPreparation() override;
    void retry() override;

private:
    void prepare();
    void launchWorker();
    void stopWorker();
    void decodeLoop(quint64 runId, const QString& modelPath);
    void postStatus(quint64 runId, const QString& status, bool failure);
    void setStatus(const QString& status);

    std::unique_ptr<DeepFistModelAssets> m_assets;
    QString m_directory;

    // Owner thread.
    bool m_started{false};
    bool m_preparing{false};
    bool m_canRetry{false};
    QString m_status;
    QString m_detail;
    quint64 m_runId{0};

    // Worker-owned while it runs; the join in stopWorker() hands it back.
    std::unique_ptr<DeepCwEngine> m_engine;
    std::atomic<bool> m_loaded{false};
    std::atomic<bool> m_workerRun{false};
    std::atomic<bool> m_resetRequested{false};
    std::atomic<float> m_pitch{0.0f};
    QThread* m_worker{nullptr};

    // Mono float32 @24 kHz handoff ring, capped at kRingCapacity samples.
    QMutex m_ringMutex;
    std::vector<float> m_ring;
    static constexpr std::size_t kRingCapacity = 24000 * 4;
};

} // namespace AetherSDR
