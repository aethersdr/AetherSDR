#pragma once

#include <QMetaType>
#include <QtGlobal>

#include <atomic>
#include <memory>

namespace AetherSDR {

// A queued DAX packet retains the exact stream incarnation that admitted it.
// Revocation is shared across the audio and network threads; a later grant
// never makes an old packet usable again. An empty permit is for remote mic.
class DaxTxAudioPermit {
public:
    static DaxTxAudioPermit blocked()
    {
        DaxTxAudioPermit permit;
        permit.m_required = true;
        return permit;
    }

    static DaxTxAudioPermit confirmed(quint32 streamId)
    {
        DaxTxAudioPermit permit = blocked();
        if (streamId != 0) {
            permit.m_state = std::make_shared<State>(streamId);
        }
        return permit;
    }

    // A revocable no-stream snapshot allows one original input to request
    // allocation. It never permits audio, and allocation retires the snapshot.
    static DaxTxAudioPermit unallocated()
    {
        DaxTxAudioPermit permit = blocked();
        permit.m_state = std::make_shared<State>(0);
        return permit;
    }

    bool allowsStreamCreation() const
    {
        return m_required && m_state && m_state->streamId == 0
            && m_state->live.load(std::memory_order_acquire);
    }

    bool permits() const
    {
        return !m_required || (m_state && m_state->streamId != 0
            && m_state->live.load(std::memory_order_acquire));
    }

    bool permitsStream(quint32 streamId) const
    {
        return m_required && m_state && m_state->streamId == streamId && permits();
    }

    void revoke() const
    {
        if (m_state) {
            m_state->live.store(false, std::memory_order_release);
        }
    }

    bool sameGrant(const DaxTxAudioPermit& other) const
    {
        return m_required == other.m_required && m_state == other.m_state;
    }

private:
    struct State {
        explicit State(quint32 id) : streamId(id) {}
        quint32 streamId;
        std::atomic<bool> live{true};
    };
    bool m_required{false};
    std::shared_ptr<State> m_state;
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::DaxTxAudioPermit)
