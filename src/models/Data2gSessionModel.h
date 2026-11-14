#pragma once

#include "core/PcmFrame.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>

namespace AetherSDR {

// Where a Data2G transmission goes. The session model hands each one over
// whole and never touches a radio itself: the sink is the only thing that can
// key, and it does so behind the radio's own TX admission.
class Data2gTransmitSink {
public:
    virtual ~Data2gTransmitSink() = default;

    // Non-empty: why this station may not transmit now. Asked when the
    // operator presses Connect, before anything is armed.
    virtual QString startRefusal() const = 0;
    // Key, play this audio (24 kHz stereo float32), unkey. False, with a
    // reason, when it cannot begin. After true, onFinished is called once.
    virtual bool start(const QByteArray& float32Stereo24k, QString& error) = 0;
    // Unkey now and play nothing more.
    virtual void stop() = 0;
    virtual bool isActive() const = 0;

    // Set by the model. `ok` false: the transmission failed or was cut short
    // by something other than the model's own stop().
    std::function<void(bool ok, const QString& why)> onFinished;
};

// Data2G connected mode for AetherModem (RFC #7159, Stage 2): the admission
// gate between the session engine and the transmitter.
//
// The engine decides when an ARQ session wants to transmit. This model
// decides whether it may. A transmission reaches the sink only while the
// model is armed, and it is armed only by connectTo(), for that one session:
//
//  - Unarmed, every transmission is dropped, whatever the engine decided.
//  - The arm ends when the session ends (plus the short closing-ID window),
//    on abortSession(), when the receive source changes or is retuned, when a
//    transmission fails to start, and when the watchdog fires. It is never
//    restored without a new connectTo().
//  - The watchdog enforces upstream's own two limits from outside the engine:
//    the session is aborted when its peer has not been heard for the
//    link-lost time, or it has carried no data for the idle time. There is no
//    limit on total session length (maintainer ruling, 2026-11-14).
//  - AETHER_AUTOMATION_NO_TX refuses the arm outright.
//
// The model lives on its owner's thread; the engine runs on a worker thread
// that exists only while a session does.
//
// In a build without the Data2G core (ENABLE_DATA2G=OFF) the model exists,
// available() is false, and connectTo() always refuses.
class Data2gSessionModel final : public QObject {
    Q_OBJECT
public:
    static constexpr int kSampleRateHz = 24000;
    static constexpr double kMinDriveDb = -20.0;
    static constexpr double kMaxDriveDb = 0.0;

    enum class State { Idle, Connecting, Connected, Disconnecting };
    Q_ENUM(State)

    // Upstream closes an idle session after 300 s and a lost link after 90 s.
    // The watchdog's limits sit a little past those, so the engine's own
    // graceful close goes first and this one catches an engine that did not.
    struct Limits {
        qint64 idleMs = 315'000;
        qint64 linkLostMs = 105'000;
        qint64 connectingMs = 120'000;
        qint64 closingMs = 8'000;      // the closing-ID window, then disarm
        qint64 maxBacklogMs = 5'000;   // audio waiting for the engine, at most
    };

    struct Stats {
        quint64 transmissions = 0;   // handed to the sink
        quint64 dropped = 0;         // refused by the gate
        quint64 paddedMs = 0;        // silence fed in place of missing audio
    };

    explicit Data2gSessionModel(Data2gTransmitSink* sink, QObject* parent = nullptr);
    Data2gSessionModel(Data2gTransmitSink* sink, Limits limits, std::function<qint64()> clockMs,
                       QObject* parent = nullptr);
    ~Data2gSessionModel() override;

    static bool available();

    void setMyCall(const QString& call);
    QString myCall() const;

    // The operator's Connect. Arms the gate for this one session and starts
    // it. False, with a reason, when refused; nothing is armed then.
    bool connectTo(const QString& peerCall, int bandwidthHz, QString& error);
    // Graceful: the peer is told and queued data still goes.
    void disconnectSession();
    // Now: unkey, drop the session, disarm.
    void abortSession(const QString& reason);
    bool sendData(const QByteArray& bytes);

    void setDriveDb(double db);   // clamped to kMinDriveDb..kMaxDriveDb; default 0
    double driveDb() const;

    // The bound receiver's continuous 24 kHz mono audio. A block from a
    // different source than the previous one aborts a session.
    void feedFrom(const PcmStreamDescriptor& source, const QVector<float>& mono);
    // The bound receiver was retuned or changed mode: aborts a session.
    void receiverChanged();

    State state() const;
    bool isArmed() const;
    QString peer() const;
    Stats stats() const;

    // For tests: run everything queued for the engine and deliver its
    // results, synchronously.
    void settle();

signals:
    void stateChanged(AetherSDR::Data2gSessionModel::State state);
    void armedChanged(bool armed);
    // As the engine words them: "CONNECTED W1AW", "DISCONNECTED TIMEOUT", ...
    void sessionEvent(const QString& event);
    void dataReceived(const QByteArray& bytes);
    void sessionAborted(const QString& reason);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace AetherSDR
