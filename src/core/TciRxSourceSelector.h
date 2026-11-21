#pragma once

#include <QHash>
#include <QtGlobal>

namespace AetherSDR {

// Chooses, per slice, which stream feeds the TCI slice route when the
// operator selects processed (AetherRX DSP) RX audio (#6864): the processed
// tap while it is flowing, the raw slice stream otherwise. Raw is the default
// and the fallback. When processed audio stops (PC audio path idle, TX, an NR
// processor still preparing, unsupported rate), raw takes the route back as
// soon as a raw frame arrives more than kStaleMs after the last processed
// one, so the hand-back lands within the 500 ms the ruling allows.
//
// Pure decision logic with an explicit clock, so it is testable without a
// socket or an audio device. Single-threaded: TciServer's thread only.
class TciRxSourceSelector {
public:
    static constexpr qint64 kStaleMs = 400;

    enum class Action {
        Drop,      // do not forward this frame
        Forward,   // forward it on the current route
        Handover,  // the route changes source: release it, then forward
    };

    void setProcessed(bool on)
    {
        m_processed = on;
        if (!on) { m_slices.clear(); }
    }
    bool processed() const { return m_processed; }

    // A raw slice frame arrived.
    Action onRaw(int sliceId, qint64 nowMs)
    {
        auto it = m_slices.find(sliceId);
        if (it == m_slices.end() || !it->processedOwns) { return Action::Forward; }
        if (m_processed && nowMs - it->lastProcessedMs <= kStaleMs) { return Action::Drop; }
        it->processedOwns = false;
        return Action::Handover;
    }

    // A processed block for this slice arrived.
    Action onProcessed(int sliceId, qint64 nowMs)
    {
        if (!m_processed) { return Action::Drop; }
        Slice& slice = m_slices[sliceId];
        slice.lastProcessedMs = nowMs;
        if (slice.processedOwns) { return Action::Forward; }
        slice.processedOwns = true;
        return Action::Handover;
    }

    bool processedOwns(int sliceId) const
    {
        const auto it = m_slices.constFind(sliceId);
        return it != m_slices.cend() && it->processedOwns;
    }

    void forget(int sliceId) { m_slices.remove(sliceId); }
    void clear() { m_slices.clear(); }

private:
    struct Slice {
        qint64 lastProcessedMs{0};
        bool processedOwns{false};
    };
    bool m_processed{false};
    QHash<int, Slice> m_slices;
};

} // namespace AetherSDR
