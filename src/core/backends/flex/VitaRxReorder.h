#pragma once

#include "core/VitaBinCoverage.h"

#include <QByteArray>
#include <QtGlobal>

#include <array>
#include <optional>
#include <algorithm>
#include <utility>
#include <vector>

namespace AetherSDR {

// A bounded receive window. Ordered packets pass immediately; a gap waits at
// most 30 ms for reordered packets before its queued successors are released.
// Opus timestamps and display frame/fragment positions disambiguate forward
// gaps from stale packets. PCM firmware with constant timestamps uses the
// 4-bit count and recent resolution history; a full lost cycle is ambiguous.
class VitaRxReorder {
public:
    static constexpr qint64 kWaitMs = 30;
    static constexpr int kSequenceCount = 16;
    static constexpr qsizetype kMaxPacketBytes = 65507;

    struct Key {
        enum class Kind { Ordered, OpusTimestamp, DisplayFrame };
        quint32 major = 0;
        quint64 minor = 0;
        bool valid = false;
        bool allowConstantFallback = false;
        Kind kind = Kind::Ordered;

        bool after(const Key& other) const
        {
            return major == other.major ? minor > other.minor
                : static_cast<quint32>(major - other.major) < 0x80000000u;
        }
        bool restartsBefore(const Key& other) const
        {
            if (after(other)) {
                return false;
            }
            const quint32 behind = other.major - major;
            if (kind == Kind::OpusTimestamp) {
                // FlexLib 4.3.16 RXAudioStream (SMART-12738) recovers from
                // clock steps strictly greater than 60 seconds backwards.
                return behind > 60 || (behind == 60 && minor < other.minor);
            }
            return kind == Kind::DisplayFrame && behind > kVitaLateFrameLookback;
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
    qint64 deadlineMs() const
    {
        if (m_pending.empty()) {
            return -1;
        }
        const auto earliest = std::min_element(m_pending.begin(), m_pending.end(),
            [](const Pending& a, const Pending& b) { return a.receivedMs < b.receivedMs; });
        return earliest->receivedMs + kWaitMs;
    }

    Result add(int sequence, Key key, qint64 nowMs, QByteArray packet = {},
               qsizetype maxQueuedBytes = kMaxPacketBytes * (kSequenceCount - 1))
    {
        if (sequence >= 0 && sequence < kSequenceCount && packet.size() <= kMaxPacketBytes
            && m_useKeys && key.valid && m_lastKey.valid && key.restartsBefore(m_lastKey)) {
            // Pending packets belong to the old clock/counter incarnation.
            reset();
        }
        Result result = expire(nowMs);
        if (sequence < 0 || sequence >= kSequenceCount || packet.size() > kMaxPacketBytes) {
            result.rejected = true;
            return result;
        }
        if (!m_useKeys) {
            key.valid = false;
        }
        if (m_lastSequence < 0) {
            release({sequence, key, std::move(packet), nowMs}, 0, nowMs, result);
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
        bool withinWindow = false;
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
            withinWindow = distance <= newestDistance || (advance > 0 && advance < 8);
            // A keyless forward window can hold the entire count cycle.
            const bool filledWindow = !(key.valid && m_lastKey.valid)
                && newestDistance == kSequenceCount - 1 && advance > 0 && advance < 8;
            if (crossedCycle || filledWindow) {
                drain(nowMs, DrainMode::All, result);
                withinWindow = false;
            }
        }
        const int distance = (sequence - m_lastSequence - 1) & 0x0F;
        if (!(key.valid && m_lastKey.valid) && !withinWindow && distance >= 8
            && m_resolvedMs[sequence] && nowMs - *m_resolvedMs[sequence] < kWaitMs) {
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
            release({sequence, key, std::move(packet), nowMs}, 0, nowMs, result);
            drain(nowMs, DrainMode::Contiguous, result);
        } else {
            if (packet.size() > std::max(qsizetype{0}, maxQueuedBytes - m_queuedBytes)) {
                result.rejected = true;
                return result;
            }
            m_queuedBytes += packet.size();
            m_pending.push_back({sequence, key, std::move(packet), nowMs});
        }
        return result;
    }

    Result expire(qint64 nowMs)
    {
        Result result;
        if (!m_pending.empty() && nowMs >= deadlineMs()) {
            drain(nowMs, DrainMode::Expired, result);
        }
        return result;
    }

private:
    enum class DrainMode { Contiguous, Expired, All };

    struct Pending {
        int sequence;
        Key key;
        QByteArray packet;
        qint64 receivedMs;
    };

    void release(Pending pending, int missed, qint64 nowMs, Result& result)
    {
        result.lost += missed;
        result.breaks += missed > 0 ? 1 : 0;
        result.ready[result.count++] = {std::move(pending.packet), missed};
        m_lastSequence = pending.sequence;
        m_lastKey = pending.key;
        for (int behind = 0; behind <= missed; ++behind) {
            m_resolvedMs[(pending.sequence - behind) & 0x0F] = nowMs;
        }
    }

    void drain(qint64 nowMs, DrainMode mode, Result& result)
    {
        while (!m_pending.empty()) {
            const auto first = std::min_element(m_pending.begin(), m_pending.end(),
                [this](const Pending& a, const Pending& b) {
                    return ((a.sequence - m_lastSequence - 1) & 0x0F)
                        < ((b.sequence - m_lastSequence - 1) & 0x0F);
                });
            const int missed = (first->sequence - m_lastSequence - 1) & 0x0F;
            if (missed > 0 && (mode == DrainMode::Contiguous
                || (mode == DrainMode::Expired && nowMs < deadlineMs()))) {
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
    std::array<std::optional<qint64>, kSequenceCount> m_resolvedMs;
    qsizetype m_queuedBytes = 0;
    std::vector<Pending> m_pending;
};

} // namespace AetherSDR
