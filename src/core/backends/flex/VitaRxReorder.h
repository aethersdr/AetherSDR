#pragma once

#include <QByteArray>
#include <QtGlobal>

#include <array>
#include <algorithm>
#include <utility>
#include <vector>

namespace AetherSDR {

// A bounded receive window. Ordered packets pass immediately; a gap waits at
// most 30 ms for reordered packets before its queued successors are released.
// Opus timestamps and display frame/fragment positions disambiguate forward
// gaps from stale packets. PCM firmware with constant timestamps uses the
// 4-bit count, whose half-cycle ambiguity cannot identify arbitrary long loss.
class VitaRxReorder {
public:
    static constexpr qint64 kWaitMs = 30;
    static constexpr int kSequenceCount = 16;
    static constexpr qsizetype kMaxPacketBytes = 65507;

    struct Key {
        quint32 major = 0;
        quint64 minor = 0;
        bool valid = false;
        bool allowConstantFallback = false;

        bool after(const Key& other) const
        {
            return major == other.major ? minor > other.minor
                : static_cast<quint32>(major - other.major) < 0x80000000u;
        }
        bool same(const Key& other) const
        {
            return major == other.major && minor == other.minor;
        }
    };

    struct Delivery {
        QByteArray packet;
        int missed = 0;
    };
    struct Result {
        std::array<Delivery, kSequenceCount> ready;
        int count = 0;
        int lost = 0;
        int breaks = 0;
        bool reordered = false;
        bool duplicate = false;
        bool rejected = false;
    };

    void reset() { *this = VitaRxReorder{}; }
    qsizetype queuedBytes() const { return m_queuedBytes; }
    qint64 deadlineMs() const { return m_pending.empty() ? -1 : m_gapSinceMs + kWaitMs; }

    Result add(int sequence, Key key, qint64 nowMs, QByteArray packet = {})
    {
        Result result = expire(nowMs);
        if (sequence < 0 || sequence >= kSequenceCount || packet.size() > kMaxPacketBytes) {
            result.rejected = true;
            return result;
        }
        if (!m_useKeys) {
            key.valid = false;
        }
        if (m_lastSequence < 0) {
            release({sequence, key, std::move(packet)}, 0, nowMs, result);
            return result;
        }
        if (key.valid && m_lastKey.valid) {
            if (key.allowConstantFallback && key.same(m_lastKey) && sequence != m_lastSequence) {
                // Some firmware repeats a placeholder timestamp on every PCM
                // packet. A non-unique key must not stop an otherwise live stream.
                m_useKeys = false;
                m_lastKey.valid = false;
                key.valid = false;
            } else if (!key.after(m_lastKey)) {
                result.duplicate = key.same(m_lastKey);
                result.reordered = !result.duplicate;
                return result;
            }
        }
        if (!m_pending.empty()) {
            const auto newest = std::max_element(m_pending.begin(), m_pending.end(),
                [this](const Pending& a, const Pending& b) {
                    return ((a.sequence - m_lastSequence - 1) & 0x0F)
                        < ((b.sequence - m_lastSequence - 1) & 0x0F);
                });
            const int distance = (sequence - m_lastSequence - 1) & 0x0F;
            const int newestDistance = (newest->sequence - m_lastSequence - 1) & 0x0F;
            const int advance = (sequence - newest->sequence) & 0x0F;
            const bool crossedCycle = key.valid && newest->key.valid
                && key.after(newest->key) && distance <= newestDistance;
            const bool filledWindow = !(key.valid && m_lastKey.valid)
                && advance > 0 && advance < 8
                && (distance >= 8 || (newestDistance >= 8 && distance <= newestDistance));
            if (crossedCycle || filledWindow) {
                drain(nowMs, true, result);
            }
        }
        const int distance = (sequence - m_lastSequence - 1) & 0x0F;
        if (!(key.valid && m_lastKey.valid) && distance >= 8
            && nowMs - m_lastReleaseMs < kWaitMs) {
            result.duplicate = sequence == m_lastSequence;
            result.reordered = !result.duplicate;
            return result;
        }
        const auto duplicate = std::find_if(m_pending.begin(), m_pending.end(),
            [sequence](const Pending& pending) { return pending.sequence == sequence; });
        if (duplicate != m_pending.end()) {
            result.duplicate = true;
            return result;
        }
        result.reordered = std::any_of(m_pending.begin(), m_pending.end(),
            [this, distance](const Pending& pending) {
                return ((pending.sequence - m_lastSequence - 1) & 0x0F) > distance;
            });
        if (distance == 0) {
            release({sequence, key, std::move(packet)}, 0, nowMs, result);
            drain(nowMs, false, result);
        } else {
            if (m_pending.empty()) {
                m_gapSinceMs = nowMs;
            }
            m_queuedBytes += packet.size();
            m_pending.push_back({sequence, key, std::move(packet)});
        }
        return result;
    }

    Result expire(qint64 nowMs)
    {
        Result result;
        if (!m_pending.empty() && nowMs >= deadlineMs()) {
            drain(nowMs, true, result);
        }
        return result;
    }

private:
    struct Pending {
        int sequence;
        Key key;
        QByteArray packet;
    };

    void release(Pending pending, int missed, qint64 nowMs, Result& result)
    {
        result.lost += missed;
        result.breaks += missed > 0 ? 1 : 0;
        result.ready[result.count++] = {std::move(pending.packet), missed};
        m_lastSequence = pending.sequence;
        m_lastKey = pending.key;
        m_lastReleaseMs = nowMs;
    }

    void drain(qint64 nowMs, bool skipGaps, Result& result)
    {
        while (!m_pending.empty()) {
            const auto first = std::min_element(m_pending.begin(), m_pending.end(),
                [this](const Pending& a, const Pending& b) {
                    return ((a.sequence - m_lastSequence - 1) & 0x0F)
                        < ((b.sequence - m_lastSequence - 1) & 0x0F);
                });
            const int missed = (first->sequence - m_lastSequence - 1) & 0x0F;
            if (missed > 0 && !skipGaps) {
                break;
            }
            m_queuedBytes -= first->packet.size();
            Pending pending = std::move(*first);
            m_pending.erase(first);
            release(std::move(pending), missed, nowMs, result);
        }
    }

    int m_lastSequence = -1;
    Key m_lastKey;
    bool m_useKeys = true;
    qint64 m_lastReleaseMs = 0;
    qint64 m_gapSinceMs = 0;
    qsizetype m_queuedBytes = 0;
    std::vector<Pending> m_pending;
};

} // namespace AetherSDR
