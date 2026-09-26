#pragma once

#include <QtGlobal>

namespace AetherSDR {

// Producer frames carry sample positions, not capture UTC. This preserves the
// existing arrival-at-input-end clock estimate while removing the local rate
// converter's acoustic delay and batching wait. It makes no RF/transport-latency
// claim. Decoder positions always remain local 24 kHz sample indices.
class ClockSampleTimeline final {
public:
    bool anchor(quint64 segmentOrigin, quint64 inputEnd, int inputRate,
                int groupDelayInputFrames, qint64 arrivalMs)
    {
        if (inputEnd < segmentOrigin || (inputRate != 24000 && inputRate != 48000)
            || groupDelayInputFrames < 0) {
            reset();
            return false;
        }
        // Subtract positions before conversion: even an origin near UINT64_MAX
        // retains the precision of a small local sample interval.
        const long double relativeEnd = inputEnd - segmentOrigin;
        m_originHostMs = static_cast<long double>(arrivalMs)
            - (relativeEnd + groupDelayInputFrames) * 1000.0L / inputRate;
        return true;
    }

    double hostMsAtSample(qint64 decoderSample) const
    {
        return static_cast<double>(m_originHostMs
            + static_cast<long double>(decoderSample) * 1000.0L / 24000.0L);
    }

    void reset() { m_originHostMs = 0.0L; }

private:
    long double m_originHostMs = 0.0L;
};

} // namespace AetherSDR
