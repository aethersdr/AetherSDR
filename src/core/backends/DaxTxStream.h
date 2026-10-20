#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include <QMetaType>
#include <QString>
#include <QtGlobal>

namespace AetherSDR {

// A bounded mutation may wait in a transport queue. Copies share revocation,
// and the immutable steady-clock deadline cannot be renewed by queued work.
// This is terminal-write admission only; it conveys no transmit authority.
class DaxTxRequestGuard {
public:
    DaxTxRequestGuard() = default;

    static DaxTxRequestGuard create(int timeoutMs)
    {
        if (timeoutMs <= 0) {
            return {};
        }
        return DaxTxRequestGuard(std::make_shared<State>(
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs)));
    }

    bool permits() const
    {
        return m_state && m_state->live.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < m_state->deadline;
    }

    void revoke() const
    {
        if (m_state) {
            m_state->live.store(false, std::memory_order_release);
        }
    }

private:
    struct State {
        explicit State(std::chrono::steady_clock::time_point until) : deadline(until) {}
        std::atomic<bool> live{true};
        const std::chrono::steady_clock::time_point deadline;
    };
    explicit DaxTxRequestGuard(std::shared_ptr<State> state) : m_state(std::move(state)) {}
    std::shared_ptr<State> m_state;
};

// Selection of one client-owned audio stream is independent of the radio's
// global DAX input setting and grants no permission to key the transmitter.
struct DaxTxOwnershipRequest {
    quint32 streamId{0};
    bool take{false};
    DaxTxRequestGuard guard;
};

using DaxTxOwnershipCompletion =
    std::function<void(bool accepted, const QString& reason)>;

// Omitted fields are deltas. Explicit malformed fields revoke the corresponding
// observation; they must not preserve an earlier, usable identity or TX bit.
struct DaxTxStreamDelta {
    quint32 streamId{0};
    bool removed{false};
    std::optional<quint32> clientHandle;
    std::optional<bool> tx;
    bool invalidOwner{false};
    bool invalidTx{false};
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::DaxTxStreamDelta)
