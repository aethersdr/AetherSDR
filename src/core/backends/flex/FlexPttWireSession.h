#pragma once

#include "FlexPttStopTracker.h"
#include "core/backends/IndependentTxControl.h"

namespace AetherSDR {

// Lives exclusively on RadioConnection's existing transport thread. The
// injected writer is the terminal socket write, not a command queue. Tests
// exercise this same composition without a socket or simulated firmware peer.
class FlexPttWireSession final {
public:
    using Writer = std::function<bool(quint32, const QString&)>;
    using EvidenceSink = std::function<void(const TxStopEvidence&)>;
    FlexPttWireSession(Writer writer, EvidenceSink sink);
    ~FlexPttWireSession();
    // TCP API version, not the firmware build or model. See the evidence doc.
    [[nodiscard]] static bool supportsProtocol(QStringView version);
    void reset(quint64 session, quint32 handle);
    void disconnect();
    // Reject changed/malformed prologue without losing the original unkey path.
    void rejectProtocol();
    void key(quint32 sequence, const TxCoordinator::Command& command, qint64 now);
    void stop(quint32 sequence, const TxCoordinator::Operation& operation,
              const TxCoordinator::StopRequest& request, qint64 now);
    void observe(QStringView line, qint64 now);
    void otherCommand(const QString& command, qint64 now);
    void poll(qint64 now);
    // The only cross-thread query; all mutation and other inspection is local.
    [[nodiscard]] bool ready() const { return m_ready.load(std::memory_order_acquire); }
    [[nodiscard]] bool needsPolling() const;

private:
    void invalidateEvidence();
    void publish(qint64 now);
    FlexPttStopTracker::Stamp stamp();
    Writer m_writer;
    EvidenceSink m_sink;
    FlexPttStopTracker m_tracker;
    quint64 m_session{0};
    quint64 m_ordinal{0};
    TxCoordinator::Operation m_operation;
    bool m_enteredKeyWrite{false};
    TxCoordinator::StopRequest m_localStop;
    std::atomic<bool> m_ready{false};
    std::shared_ptr<std::atomic<bool>> m_evidence;
};

} // namespace AetherSDR
