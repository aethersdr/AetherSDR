#pragma once
#ifdef HAVE_WEBSOCKETS

#include "PcmFrame.h"
#include "TciClient.h"
#include "TciRxConverter.h"
#include "TxCoordinator.h"
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QPointer>
#include <QTimer>
#include <deque>
#include <functional>
#include <optional>

class QWebSocket;
class QWebSocketServer;

namespace AetherSDR {
class Resampler;

struct TciRxBinding {
    quint64 key{0};
    int sliceId{-1};
    int trx{-1};
    int gainChannel{0};
    // Revoked synchronously by the model owner; checked at final delivery.
    std::shared_ptr<std::atomic<bool>> alive;
    bool current() const { return alive && alive->load(std::memory_order_acquire); }
};

struct TciStreamConfig {
    std::shared_ptr<TciClientLifetime> lifetime;
    bool audio{false};
    int receiver{-1};
    int rate{48000};
    int channels{2};
    int format{3};
    quint64 generation{0};
};

// Owns sockets, PCM conversion and chrono. No model, backend, settings or UI
// pointer may enter this object. Only copied control snapshots, revocation
// handles and owning PCM cross its boundary. It never waits for the controller.
class TciIoWorker final : public QObject {
    Q_OBJECT
    friend class TciRxAudioTest;
    friend class TciServerReviewTest;
    friend class TxOperationIntegrationTestAccess;
public:
    explicit TciIoWorker(QObject* parent = nullptr);
    ~TciIoWorker() override;
    bool start(quint16 port);
    void stop();
    quint16 port() const;
    // Bounded mailbox; safe from any producer thread. One scheduled drain,
    // finite batches, no unbounded Qt event per PCM packet.
    bool post(std::function<void()> work, qsizetype bytes = 0);
    void activate(quint64 id, const TxCoordinator::Producer& producer);
    void acknowledgeText(quint64 id);
    void sendText(quint64 id, const QString& message);
    qint64 sendBinary(quint64 id, const QByteArray& message);
    void closeClient(quint64 id, QWebSocketProtocol::CloseCode code, const QString& reason);
    void configureClient(const TciStreamConfig& config);
    void removeClient(quint64 id);
    void setRxBindings(const QHash<quint64, TciRxBinding>& bindings);
    void receivePcm(quint64 key, const PcmFrame& frame);
    void retireRxRoute(quint64 key);
    void setRxGain(int channel, float gain);
    void setTxGain(float gain, int overflowMode);
    void prepareTx(quint64 id, const TxCoordinator::Context& context);
    void startChrono(quint64 id, int trx, const TxCoordinator::Context& context);
    void stopChrono();
    void receiveBinary(quint64 id, const QByteArray& data);
    QJsonObject chronoSnapshot() const;
    void noteTxChronoPoll(qint64 gapNs, int framesSent);
    void resetTxChronoStallStats();
    void acknowledgeLevel(int channel);
    QJsonObject cachedChronoSnapshot() const;
    static quint64 rxRouteKey(bool dax, int id)
    { return (quint64(dax) << 32) | static_cast<quint32>(id); }

signals:
    void audioStopped(quint64 id, quint64 generation);
    void clientOpened(std::shared_ptr<TciClientLifetime> lifetime, QHostAddress address, quint16 port);
    void clientClosed(quint64 id, int closeCode, int error, const QString& errorText);
    void textReceived(quint64 id, const QString& text, const AetherSDR::TxCoordinator::Request& input);
    void rxLevel(int channel, float rms);
    void txLevel(float rms);
    void txPcmReady(const QByteArray& pcm, const AetherSDR::TxCoordinator::Context& context);

private:
    struct Client {
        QPointer<QWebSocket> socket;
        std::shared_ptr<TciClientLifetime> lifetime;
        TxCoordinator::Producer producer;
        bool activated{false};
        bool textInFlight{false};
        struct Text { QString message; TxCoordinator::Request input; bool captured{false}; };
        std::deque<Text> texts;
        qsizetype textBytes{0};
        int error{-1};
        QString errorText;
    };
    struct RxClient {
        TciStreamConfig config;
        QHash<quint64, std::shared_ptr<TciRxConverter>> converters;
    };
    struct RxRoute {
        std::optional<PcmFrame> pin;
        std::shared_ptr<std::atomic<bool>> owner;
        quint64 nextSample{0};
        bool retired{false};
    };
    void acceptConnections();
    void publishRxLevel(int channel, float rms);
    void publishTxLevel(float rms);
    void publishTelemetry();
    bool m_rxLevelPending[8]{};
    bool m_txLevelPending{false};
    mutable QMutex m_diagnosticsMutex;
    QJsonObject m_diagnostics;
    void dispatchText(quint64 id);
    void captureTextInputs(Client& client);
    void drain();
    void resetRxRoute(quint64 key);
    bool rxDeliveryCurrent(quint64 id, quint64 generation, quint64 key,
                           const PcmFrame& frame, const std::shared_ptr<TciRxConverter>& converter) const;
    static QByteArray encodeRxAudio(int trx, int rate, int channels, int format,
                                   const QVector<float>& stereo, float gain);
    void sendTxChronoFrame(quint64 id);
    void logTxAudioSummary(const char* reason);
    QWebSocketServer* m_server{nullptr};
    QHash<quint64, Client> m_clients;
    QHash<quint64, RxClient> m_rxClients;
    QHash<quint64, TciRxBinding> m_bindings;
    QHash<quint64, RxRoute> m_rxRoutes;
    PcmFrameGate m_rxGate;
    bool m_rxProcessing{false};
    std::function<qint64(quint64, const QByteArray&)> m_rxSend;
    std::function<qint64(quint64)> m_rxBacklog;
    std::function<void(quint64, QWebSocketProtocol::CloseCode, const QString&)> m_rxClose;
    static constexpr qint64 kMaxRxBacklogBytes = 256 * 1024;
    float m_rxChannelGain[8]{1,1,1,1,1,1,1,1};
    qint64 m_rxAudioFramesSent{0};
    QMutex m_mailboxMutex;
    struct Work { std::function<void()> run; qsizetype bytes; };
    std::deque<Work> m_mailbox;
    qsizetype m_mailboxBytes{0};
    bool m_drainScheduled{false};
    bool m_overloaded{false};
    bool m_accepting{true};
    QTimer m_txChronoTimer;
    quint64 m_txClient{0};
    int m_txChronoTrx{0};
    TxCoordinator::Context m_tciTxContext;
    std::unique_ptr<Resampler> m_txResampler;
    QElapsedTimer m_txChronoClock;
    QElapsedTimer m_txChronoSessionClock;
    qint64 m_txChronoAccumNs{0};
    qint64 m_txChronoRequestedFrames{0};
    qint64 m_txChronoPollCount{0};
    qint64 m_txChronoMaxPollGapNs{0};
    qint64 m_txChronoLatePolls{0};
    qint64 m_txChronoCatchUpBursts{0};
    qint64 m_txChronoCatchUpFrames{0};
    int m_txChronoMaxCatchUp{0};
    float m_txGain{1};
    enum class OverflowMode { Clip, NaNGuard, Measure };
    OverflowMode m_overflowMode{OverflowMode::Clip};
    qint64 m_txAudioBlocks{0};
    qint64 m_txInputFrames{0};
    qint64 m_txOutputFrames{0};
    qint64 m_txClipSamples{0};
    qint64 m_txAudioSampleCount{0};
    double m_txAudioSumSq{0};
    float m_txAudioPeak{0};
    bool m_txSawDuplicatedStereo{false};
};
} // namespace AetherSDR
#endif
