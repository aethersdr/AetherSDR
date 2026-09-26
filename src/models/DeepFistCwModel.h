#pragma once
#include "core/PcmFrame.h"
#include "core/deepfist/DeepFistStream.h"
#include <QObject>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <memory>
class QNetworkAccessManager;
namespace AetherSDR {
class DeepFistModelAssets;
// GUI-facing experimental RX decoder. All DSP and model state is worker-owned.
// Public methods run on the owning thread; no inference runs on that thread.
class DeepFistCwModel final : public QObject {
    Q_OBJECT
public:
    explicit DeepFistCwModel(QObject* parent = nullptr);
    DeepFistCwModel(QString directory, QString baseUrl, QNetworkAccessManager* network,
                   QObject* parent = nullptr);
    // Explicit construction seam for developer qualification; immutable while running.
    DeepFistCwModel(DeepFistStream::Parameters parameters, QObject* parent = nullptr);
    ~DeepFistCwModel() override;
    void start();
    void stop();
    void reset();
    void feed(const PcmFrame& frame);
    void cancelPreparation();
    void retry();
    bool readyForAudio() const { return m_running && m_acceptAudio.load(); }
    bool processing() const { return m_processing.load(); }
    qsizetype queuedFrames();
    bool preparing() const { return m_preparing; }
    bool canRetry() const { return m_canRetry; }
    QString detail() const { return m_detail; }
    bool isRunning() const { return m_running; }
    QString status() const { return m_status; }
    static QString modelDirectory();
    // The parameters the application runs DeepFist with (the default constructors use them).
    static DeepFistStream::Parameters appParameters();
signals:
    void textDecoded(const QString& text);
    void statusChanged(const QString& status);
private:
    struct Item { PcmFrame frame; quint64 generation; };
    void run(quint64 generation, const QString& directory);
    void postStatus(quint64 generation, const QString& status, bool failure = false);
    void setStatus(const QString& status);
    DeepFistStream::Parameters m_parameters;
    std::thread m_worker;
    std::unique_ptr<DeepFistModelAssets> m_assets;
    QString m_directory;
    QString m_detail;
    bool m_preparing = false;
    bool m_canRetry = false;
    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<Item> m_queue;
    qsizetype m_queuedFrames = 0;
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_processing{false};
    std::atomic<bool> m_acceptAudio{false};
    std::atomic<quint64> m_generation{1};
    quint64 m_runId = 0;
    bool m_running = false;
    QString m_status;
    PcmFrameGate m_gate;
    PcmStreamDescriptor m_stream;
    quint64 m_nextSample = 0;
};
}
