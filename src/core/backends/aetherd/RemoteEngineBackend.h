#pragma once

#include "core/backends/IRadioBackend.h"
#include "core/backends/aetherd/RemoteEngineCredentials.h"
#include "core/backends/aetherd/RemoteEngineScheduler.h"

#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QTimer>

#include <functional>
#include <memory>

class QWebSocket;

namespace AetherSDR::aetherd {

// Family "aetherd": desktop AetherSDR as a receive-only client of a remote
// aetherd (docs/aetherd-remote-backend.md §2). The "radio" is the engine; this
// backend never speaks a vendor protocol and never asks the engine to connect
// or release a radio. One QWebSocket on the owner's thread, no worker.
//
// The four rules that make it a mirror:
//  1. Connecting is silent: hello and resource.subscribe only. Verbs that
//     arrive before the first snapshot are dropped, never replayed.
//  2. Disconnecting only closes the socket.
//  3. Every control carries expectedRevision; one request in flight per
//     resource, latest wins per method, one retry after a conflict, and a
//     session-wide client bucket at half the engine's budget.
//  4. resource.resyncRequired triggers a fresh snapshot.
class RemoteEngineBackend final : public IRadioBackend {
    Q_OBJECT

public:
    static constexpr const char* kFamily = "aetherd";

    struct Options {
        // Milliseconds from a monotonic origin, for the client bucket.
        std::function<qint64()> clockMs;
        // The interface name that holds a local address; empty when none.
        // Injected so the WireGuard admission check is testable.
        std::function<QString(const QHostAddress&)> interfaceForLocalAddress;
    };

    explicit RemoteEngineBackend(QObject* parent = nullptr);
    RemoteEngineBackend(Options options, QObject* parent);
    ~RemoteEngineBackend() override;

    [[nodiscard]] RadioCapabilities capabilities() const override;
    void connectRadio(const RadioConnectRequest& request) override;
    void disconnectRadio() override;
    [[nodiscard]] bool isConnected() const override { return m_connected; }

    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    ReceiveDispatch requestSliceDsp(int sliceId, const SliceDspRequest& request) override;
    ReceiveDispatch requestSliceAudio(int sliceId, const SliceAudioRequest& request) override;
    ReceiveDispatch requestSliceControl(int sliceId, const SliceControlRequest& request) override;
    ReceiveDispatch requestRadioSpeakerAudio(bool on) override;
    void setCwPitch(int hz) override;
    void setPanCenter(const QString& panId, double hz, PanCenterIntent intent) override;
    void setPanBandwidth(const QString& panId, double hz) override;
    void setPanRfGain(const QString& panId, int gainDb) override;
    void setKeying(bool key, const AetherSDR::TxCoordinator::Operation& operation,
                   const AetherSDR::TxCoordinator::Completion& completion = {}) override;
    void invokeExtension(const QString& ns, const QString& verb,
                         quint64 requestId, const QVariant& arg = {}) override;

    // The record the connect path resolves from the session cache: the
    // request's serial is the engine id. Nullopt when this machine never
    // paired with it this session.
    [[nodiscard]] static std::optional<RemoteEngineRecord> recordFor(const RadioConnectRequest& request);
    // ws:// is admitted only when our end of the socket is on an up
    // tailscale*/wg* interface.
    [[nodiscard]] static bool tailnetPathAdmitted(const QHostAddress& localAddress,
        const std::function<QString(const QHostAddress&)>& interfaceFor);
    // The lowercase hex SHA-256 the pin compares.
    [[nodiscard]] static QString fingerprintOf(const QByteArray& certificateDer);

    // For tests: what has gone out on the wire after hello.
    [[nodiscard]] QStringList sentMethodsForTest() const { return m_sentMethods; }
    [[nodiscard]] RemoteEngineScheduler::Limits clientLimitsForTest() const { return m_scheduler.limits(); }
    [[nodiscard]] bool helloSentForTest() const { return m_helloSent; }
    // Feeds one frame as if the engine sent it (a forced resync, a stale event).
    void receiveTextForTest(const QString& text) { onText(text); }

private:
    enum class Phase { Idle, Opening, Hello, Live };
    struct Pending {
        enum class Kind { Hello, Subscribe, Get, Intent } kind{Kind::Intent};
        QString resourceKey;
        QString method;
        QJsonObject params;
        bool retried{false};
    };
    struct Cached {
        QJsonObject address;
        quint64 revision{0};
        QJsonObject value;
    };

    [[nodiscard]] static Options withDefaults(Options options);
    void openSocket(const RemoteEngineRecord& record, bool tailnet);
    void onSocketConnected();
    void onText(const QString& text);
    void onResponse(const QJsonObject& message);
    void onEvent(const QJsonObject& message);
    void applyBaseline(const QJsonArray& resources, bool replaceAll);
    void applyResource(const QJsonObject& address, quint64 revision, const QJsonObject& value);
    void removeResource(const QJsonObject& address);
    void publishSlice(const Cached& slice);
    void publishPan(const Cached& pan);
    void publishSession(const Cached& session);
    void refreshCapabilities();
    void fail(const QString& reason);
    void finish();
    void subscribe();
    void sendNow(Pending::Kind kind, const QString& method, const QJsonObject& params,
                 const QString& resourceKey = {}, bool retried = false);
    void pump();
    void schedulePump();

    // Queue a control for a live slice/pan/session; false (nothing sent) before
    // the first snapshot or for an unknown resource.
    bool submit(const QString& type, const QString& id, const QString& method, QJsonObject params);
    [[nodiscard]] bool outstanding(const QString& key, const QString& method) const;
    [[nodiscard]] const Cached* cached(const QString& type, const QString& id) const;

    Options m_options;
    std::unique_ptr<QWebSocket> m_socket;
    quint64 m_generation{0};
    Phase m_phase{Phase::Idle};
    bool m_connected{false};
    bool m_snapshotReady{false};
    bool m_tailnet{false};
    bool m_helloSent{false};
    QString m_pin;
    QString m_token;
    QString m_sessionId;
    quint64 m_nextId{0};
    qint64 m_lastSequence{-1};
    QHash<QString, Pending> m_pending;
    QHash<QString, Cached> m_cache;
    // resource key -> the conflicted intent (method, params) its re-read serves
    QHash<QString, std::pair<QString, QJsonObject>> m_retryIntents;
    RemoteEngineScheduler m_scheduler;
    QTimer m_pumpTimer;
    QString m_agcMode;
    QJsonObject m_capabilityJson;
    QStringList m_sentMethods;
};

} // namespace AetherSDR::aetherd
