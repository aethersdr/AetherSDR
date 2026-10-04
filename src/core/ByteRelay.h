#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <array>
#include <cstdint>

class QTimer;

namespace AetherSDR {

// One side of an opaque byte relay. Implementations wrap a transport (a
// QTcpSocket in production, a scripted fake in tests) and report readiness
// through the notify* callbacks of the ByteRelay they are attached to.
class ByteRelayEndpoint {
public:
    virtual ~ByteRelayEndpoint() = default;

    // Bytes that read() can return without blocking.
    virtual qint64 bytesAvailable() const = 0;
    virtual QByteArray read(qint64 maxBytes) = 0;
    // Accepts a prefix of data (possibly empty); -1 is a hard error.
    virtual qint64 write(const QByteArray& data) = 0;
    // Bytes accepted by write() but not yet handed to the OS.
    virtual qint64 bytesToWrite() const = 0;
    // Orderly close after the last write has been flushed.
    virtual void closeGracefully() = 0;
    // Immediate close; discards anything unsent.
    virtual void abort() = 0;
};

// Opaque bidirectional pump between two endpoints. Every byte read from one
// side is written to the other exactly once, in order, unmodified. Memory is
// bounded per direction by Limits::directionBudget, covering the relay's own
// pending queue plus the destination's unsent write buffer; when a direction
// is full the relay stops reading its source, so backpressure reaches the
// sender through the transport's own read-buffer limit.
// Design: docs/ctr2-tcp-proxy-design.md.
class ByteRelay : public QObject {
    Q_OBJECT

public:
    enum class Side { A = 0, B = 1 };

    struct Limits {
        qint64 directionBudget{256 * 1024};
        qint64 sliceBytes{64 * 1024};   // per pump before yielding to the loop
        qint64 readChunk{16 * 1024};
        int progressTimeoutMs{10000};   // armed only while bytes are pending
        int drainTimeoutMs{10000};      // after an orderly EOF
    };

    enum class EndReason {
        None,
        PeerClosed,       // orderly EOF, drain completed
        DrainTimeout,
        ProgressTimeout,
        TransportError,
        Stopped,
    };

    struct Stats {
        std::array<quint64, 2> forwarded{};  // indexed by source side
        std::array<qint64, 2> queued{};      // pending + destination unsent
    };

    ByteRelay(ByteRelayEndpoint* a, ByteRelayEndpoint* b,
              const Limits& limits, QObject* parent = nullptr);
    ~ByteRelay() override;

    void start();
    // Hard close: aborts both endpoints and discards anything queued.
    void stop();

    // Transport notifications. Calls after the relay has finished are ignored.
    void notifyReadable(Side side);
    void notifyBytesWritten(Side side);
    void notifyEndOfInput(Side side);
    void notifyError(Side side, const QString& message);
    // The destination finished its graceful close (drain complete).
    void notifyClosed(Side side);

    bool isFinished() const { return m_finished; }
    Stats stats() const;
    EndReason endReason() const { return m_endReason; }
    QString endMessage() const { return m_endMessage; }

    static constexpr Side other(Side s) { return s == Side::A ? Side::B : Side::A; }

signals:
    void statsChanged();
    // An orderly EOF began the bounded drain-then-close sequence.
    void drainStarted();
    // Emitted once. discarded[] counts bytes dropped per source side.
    void finished(AetherSDR::ByteRelay::EndReason reason, const QString& message,
                  qint64 discardedFromA, qint64 discardedFromB);

private:
    struct Direction {
        QByteArray pending;
        bool continuationScheduled{false};
    };

    static int idx(Side s) { return static_cast<int>(s); }
    ByteRelayEndpoint* endpoint(Side s) const { return m_ep[idx(s)]; }
    qint64 queuedBytes(Side src) const;
    bool flushPending(Side src);
    void pump(Side src);
    void scheduleContinuation(Side src);
    void updateProgressTimer();
    void beginDrain(Side side);
    void maybeFinishDrain();
    void finish(EndReason reason, const QString& message, bool graceful);

    std::array<ByteRelayEndpoint*, 2> m_ep;
    Limits m_limits;
    std::array<Direction, 2> m_dir;
    std::array<quint64, 2> m_forwarded{};
    std::array<bool, 2> m_inputEnded{};
    bool m_started{false};
    bool m_draining{false};
    Side m_drainSource{Side::A};
    bool m_closingGracefully{false};
    bool m_finished{false};
    EndReason m_endReason{EndReason::None};
    QString m_endMessage;
    QTimer* m_progressTimer{nullptr};
    QTimer* m_drainTimer{nullptr};
};

} // namespace AetherSDR
