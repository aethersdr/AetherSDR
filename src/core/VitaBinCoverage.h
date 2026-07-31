#pragma once

#include <QVector>

#include <algorithm>

namespace AetherSDR {

// Return the number of complete bins actually present in a VITA payload.
// Display packet headers can advertise a full fragment even when the received
// datagram ends early; callers must never mark or decode the missing tail.
inline int boundedVitaPayloadBinCount(int declaredBins,
                                      int bytesPerBin,
                                      int availableBytes)
{
    if (declaredBins <= 0 || bytesPerBin <= 0 || availableBytes < bytesPerBin) {
        return 0;
    }
    return std::min(declaredBins, availableBytes / bytesPerBin);
}

// FLEX can publish one transitional FFT after an x_pixels increase where the
// old-width prefix is valid but every newly-added bin is zero. Raw FFT zero is
// the top/max-dBm pixel, so accepting that frame creates a tall right-hand
// shelf and contaminates the display averager for several subsequent rows.
// Restrict the check to a genuine growth and require both a nearly-all-zero
// suffix and a non-zero-dominated prefix so a real saturated signal is not
// mistaken for the radio's resize placeholder.
inline bool hasZeroFilledFftGrowthSuffix(const QVector<quint16>& bins,
                                         int previousAcceptedBins)
{
    if (previousAcceptedBins <= 0
        || previousAcceptedBins >= bins.size()) {
        return false;
    }

    const int suffixBins = bins.size() - previousAcceptedBins;
    if (suffixBins < 16) {
        return false;
    }

    const int prefixZeros = static_cast<int>(std::count(
        bins.cbegin(), bins.cbegin() + previousAcceptedBins, quint16{0}));
    const int suffixZeros = static_cast<int>(std::count(
        bins.cbegin() + previousAcceptedBins, bins.cend(), quint16{0}));

    return suffixZeros * 10 >= suffixBins * 9
        && prefixZeros * 10 < previousAcceptedBins;
}

// Tracks unique bin positions while a fragmented VITA display frame is
// assembled. Packet counts cannot prove completeness because UDP duplicates or
// overlapping fragments may repeat positions while leaving a gap elsewhere.
class VitaBinCoverage
{
public:
    void reset(int totalBins)
    {
        m_received.fill(0, std::max(0, totalBins));
        m_uniqueBins = 0;
    }

    bool markRange(int firstBin, int binCount)
    {
        if (firstBin < 0 || binCount <= 0
            || firstBin > m_received.size() - binCount) {
            return false;
        }
        bool markedNewBin = false;
        for (int index = firstBin; index < firstBin + binCount; ++index) {
            if (m_received[index] == 0) {
                m_received[index] = 1;
                ++m_uniqueBins;
                markedNewBin = true;
            }
        }
        return markedNewBin;
    }

    bool isComplete() const
    {
        return !m_received.isEmpty() && m_uniqueBins == m_received.size();
    }

    int uniqueBins() const { return m_uniqueBins; }
    int totalBins() const { return m_received.size(); }

private:
    QVector<quint8> m_received;
    int m_uniqueBins{0};
};

enum class FftGrowthSuffixAction {
    Accept,
    Reject,
    EmitWithFloorSuffix,
};

// A FLEX-8600 running firmware 4.2.18 normally sends at most one expanded FFT
// frame whose newly-added suffix is zero-filled. Rejecting every such frame is
// safest for the common case, but an indefinitely repeated placeholder must not
// freeze the panadapter at its old width forever. After a short bounded wait,
// callers may emit the valid prefix with the new suffix forced to the floor;
// the first genuinely populated expanded frame resets this guard and is
// accepted normally.
class FftGrowthSuffixGuard
{
public:
    static constexpr int kRejectedFramesBeforeFloorFallback = 3;

    FftGrowthSuffixAction observe(bool zeroFilledGrowth, int totalBins)
    {
        if (!zeroFilledGrowth || totalBins <= 0) {
            reset();
            return FftGrowthSuffixAction::Accept;
        }
        if (totalBins != m_rejectedTotalBins) {
            m_rejectedTotalBins = totalBins;
            m_consecutiveRejectedFrames = 1;
        } else {
            ++m_consecutiveRejectedFrames;
        }
        return m_consecutiveRejectedFrames
                <= kRejectedFramesBeforeFloorFallback
            ? FftGrowthSuffixAction::Reject
            : FftGrowthSuffixAction::EmitWithFloorSuffix;
    }

    void reset()
    {
        m_rejectedTotalBins = 0;
        m_consecutiveRejectedFrames = 0;
    }

    int consecutiveRejectedFrames() const
    {
        return m_consecutiveRejectedFrames;
    }

private:
    int m_rejectedTotalBins{0};
    int m_consecutiveRejectedFrames{0};
};

} // namespace AetherSDR
