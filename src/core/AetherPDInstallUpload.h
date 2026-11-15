#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace AetherSDR {
// Injection seam for the fixed-port duplex stream. finishWrites sends TCP EOF
// while retaining the read side for the two authenticated acknowledgements.
class AetherPDInstallChannel : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void connectTo(const QHostAddress& address) = 0;
    virtual qint64 write(const char* bytes, qint64 size) = 0;
    virtual qint64 pendingBytes() const = 0;
    virtual qint64 availableBytes() const = 0;
    virtual QByteArray read(qint64 maximum) = 0;
    virtual bool finishWrites() = 0;
    virtual void close() = 0;
signals:
    void connected();
    void readable();
    void writable();
    void closed();
};

// Caller authenticates packages and obtains consent first. Upload verifies the
// receiver's keyed challenge and the exact input binding. Delivery is not commit.
class AetherPDInstallUpload final : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Connecting, Authenticating, Sending, AwaitingVerification, AwaitingDelivery, Finished, Unconfirmed, Canceled };
    Q_ENUM(State)
    enum class Reason { None, Deadline, InvalidClock, Transport, InvalidResponse, Authentication, TransportDestroyed };
    Q_ENUM(Reason)
    using Inputs = std::array<QByteArray, 4>; // Signed worker, approved plan, candidate, previous.
    using Clock = std::function<qint64()>;
    explicit AetherPDInstallUpload(QObject* parent = nullptr);
    AetherPDInstallUpload(AetherPDInstallChannel& channel, Clock clock, QObject* parent = nullptr);
    ~AetherPDInstallUpload() override;
    bool begin(const QHostAddress& address, const QByteArray& privateConfig, const Inputs& inputs);
    void cancel();
    void poll();
    State state() const { return m_state; }

signals:
    void inputsVerified();
    void finished(AetherSDR::AetherPDInstallUpload::State state,
                  AetherSDR::AetherPDInstallUpload::Reason reason, int deliveryResult);

private:
    void initialize();
    bool active() const;
    std::optional<qint64> now();
    bool withinDeadline();
    void receive();
    void pump();
    void end(State state, Reason reason, int result = -1);
    std::array<uint8_t, 32> mac(const char* domain, const QByteArray& extra = {}) const;
    bool authenticate(const QByteArray& bytes, qsizetype offset, const char* domain, const QByteArray& extra = {}) const;

    std::unique_ptr<AetherPDInstallChannel> m_ownedChannel;
    QPointer<AetherPDInstallChannel> m_channel;
    QTimer m_timer;
    QElapsedTimer m_clock;
    Clock m_now;
    State m_state{State::Idle};
    qint64 m_lastNow{0};
    qint64 m_deadline{0};
    std::array<uint8_t, 32> m_key{};
    QByteArray m_receipt;
    QByteArray m_challenge;
    QByteArray m_header;
    std::array<QByteArray, 5> m_output;
    std::size_t m_part{0};
    qsizetype m_offset{0};
    QByteArray m_input;
    bool m_pumping{false};
};
} // namespace AetherSDR
