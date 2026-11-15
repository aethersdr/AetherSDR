#pragma once

#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <optional>
#include <vector>

namespace AetherSDR {

enum class ReceivePresentationSource {
    Flex,
    KiwiSdr,
};

enum class ReceivePresentationSurface {
    Audio,
    Waterfall,
    Spectrum,
    Meter,
    Analyzer,
    Decoder,
    Recording,
};

enum class ReceiveSyncMode {
    Manual,
    AutoAssist,
};

enum class ReceiveSyncReference {
    Auto,
    Flex,
    KiwiSdr,
};

enum class ReceiveSyncStatus {
    Off,
    Manual,
    Searching,
    Holding,
    Locked,
    LowConfidence,
};

struct ReceiveSyncEstimate {
    int offsetMs{0};          // Positive values delay Flex relative to KiwiSDR.
    float confidence{0.0f};   // 0.0-1.0, estimator-specific.
    bool valid{false};
    int driftPpm{0};
    bool held{false};
};

struct ReceivePresentationSettings {
    bool enabled{false};
    ReceiveSyncMode mode{ReceiveSyncMode::Manual};
    ReceiveSyncReference reference{ReceiveSyncReference::Auto};
    int baseLatencyMs{360};
    int manualOffsetMs{0};    // Positive values delay Flex relative to KiwiSDR.
    int maxOffsetMs{3000};
    float autoLockConfidence{0.35f};
    ReceiveSyncEstimate autoEstimate;
};

struct ReceiveDelayBreakdown {
    int flexDelayMs{0};
    int kiwiDelayMs{0};
    int effectiveOffsetMs{0};
    ReceiveSyncStatus status{ReceiveSyncStatus::Off};
};

QString receivePresentationVisualQueueKey(ReceivePresentationSource source,
                                          ReceivePresentationSurface surface,
                                          const QString& sourceId = QString());
int receivePresentationExternalKiwiDelayMs(
    const QString& sourceId,
    const QString& delaySourceId,
    int kiwiDelayMs);
bool receivePresentationShouldPrebufferAfterDelayChange(int previousDelayMs,
                                                        int nextDelayMs,
                                                        bool sourceAudible,
                                                        bool hasQueuedAudio);

struct ReceiveSyncAudioPairEndpoint {
    int sliceId{-1};
    qint64 frequencyHz{0};
    QString kiwiProfileId;
};

struct ReceiveSyncAudioPairSelection {
    enum class State {
        None,
        Usable,
        Ambiguous,
    };

    State state{State::None};
    QString kiwiProfileId;
    int flexSliceId{-1};
    int kiwiSliceId{-1};
    qint64 frequencyHz{0};
    int audibleFlexCount{0};
    int audibleKiwiCount{0};
    int matchingPairCount{0};
    QString reason;

    bool usable() const { return state == State::Usable; }
    bool ambiguous() const { return state == State::Ambiguous; }
};

ReceiveSyncAudioPairSelection selectReceiveSyncAudioPair(
    const QVector<ReceiveSyncAudioPairEndpoint>& flexCandidates,
    const QVector<ReceiveSyncAudioPairEndpoint>& kiwiCandidates,
    qint64 frequencyToleranceHz);

class ReceivePresentationSync {
public:
    static constexpr int kMinBaseLatencyMs = 0;
    static constexpr int kMaxBaseLatencyMs = 5000;
    static constexpr int kMinOffsetMs = -5000;
    static constexpr int kMaxOffsetMs = 5000;

    ReceivePresentationSettings settings() const { return m_settings; }
    void setSettings(const ReceivePresentationSettings& settings);

    void setEnabled(bool enabled);
    void setMode(ReceiveSyncMode mode);
    void setManualOffsetMs(int offsetMs);
    void setBaseLatencyMs(int latencyMs);
    void setMaxOffsetMs(int maxOffsetMs);
    void setAutoEstimate(const ReceiveSyncEstimate& estimate);
    void clearAutoEstimate();

    ReceiveDelayBreakdown delayBreakdown() const;
    int delayMs(ReceivePresentationSource source,
                ReceivePresentationSurface surface) const;
    qint64 dueTimeMs(ReceivePresentationSource source,
                     ReceivePresentationSurface surface,
                     qint64 arrivalMs) const;
    ReceiveSyncStatus status() const { return delayBreakdown().status; }
    static int adjustedAutoOffsetMs(int currentOffsetMs,
                                    int candidateOffsetMs,
                                    bool reset);

private:
    static int clampedOffset(int offsetMs, int maxAbsMs);
    int effectiveOffsetMs() const;

    ReceivePresentationSettings m_settings;
};

struct ReceiveAudioDelayEstimate {
    int offsetMs{0};             // Positive values delay Flex relative to KiwiSDR.
    float confidence{0.0f};
    float peakCorrelation{0.0f}; // Absolute normalized correlation at the GCC-PHAT peak.
    bool valid{false};
};

class ReceiveAudioDelayEstimator {
public:
    struct Config {
        int sampleRateHz{12000};
        int maxOffsetMs{3000};
        int minOverlapMs{750};
        float minPeakCorrelation{0.20f};
        float minConfidence{0.18f};
    };

    ReceiveAudioDelayEstimator();
    explicit ReceiveAudioDelayEstimator(Config config);

    Config config() const { return m_config; }
    void setConfig(const Config& config);

    ReceiveAudioDelayEstimate estimate(const QVector<float>& flexMono,
                                       const QVector<float>& kiwiMono) const;

private:
    static void fft(std::vector<std::complex<double>>& data, bool inverse);

    Config m_config;
};

template <typename Payload>
struct ReceivePresentationQueuedItem {
    ReceivePresentationSource source{ReceivePresentationSource::Flex};
    ReceivePresentationSurface surface{ReceivePresentationSurface::Audio};
    QString sourceId;
    qint64 arrivalMs{0};
    qint64 dueMs{0};
    quint64 sequence{0};
    Payload payload;
};

template <typename Payload>
class ReceivePresentationQueue {
public:
    using Item = ReceivePresentationQueuedItem<Payload>;

    void enqueue(Item item)
    {
        const auto insertAt = std::upper_bound(
            m_items.begin(), m_items.end(), item,
            [](const Item& incoming, const Item& existing) {
                if (incoming.dueMs != existing.dueMs) {
                    return incoming.dueMs < existing.dueMs;
                }
                return incoming.sequence < existing.sequence;
            });
        m_items.insert(insertAt, std::move(item));
    }

    QVector<Item> releaseDue(
        qint64 nowMs,
        qsizetype maxItems = std::numeric_limits<qsizetype>::max())
    {
        QVector<Item> released;
        while (!m_items.isEmpty() && m_items.constFirst().dueMs <= nowMs
               && released.size() < maxItems) {
            released.append(std::move(m_items.first()));
            m_items.removeFirst();
        }
        return released;
    }

    void clear() { m_items.clear(); }
    bool isEmpty() const { return m_items.isEmpty(); }
    qsizetype size() const { return m_items.size(); }
    template <typename Predicate>
    qsizetype removeIf(Predicate predicate)
    {
        const qsizetype before = m_items.size();
        const auto firstRemoved =
            std::remove_if(m_items.begin(), m_items.end(), predicate);
        m_items.erase(firstRemoved, m_items.end());
        return before - m_items.size();
    }

    void trimToSize(qsizetype maxItems)
    {
        maxItems = std::max<qsizetype>(0, maxItems);
        while (m_items.size() > maxItems) {
            m_items.removeFirst();
        }
    }

    std::optional<qint64> nextDueMs() const
    {
        if (m_items.isEmpty()) {
            return std::nullopt;
        }
        return m_items.constFirst().dueMs;
    }

private:
    QVector<Item> m_items;
};

} // namespace AetherSDR
