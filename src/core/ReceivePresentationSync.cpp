#include "ReceivePresentationSync.h"

#include <numeric>

namespace AetherSDR {
namespace {

constexpr float kEpsilon = 1.0e-12f;
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr int kEnvelopeFrameMs = 10;
constexpr int kEnvelopeWaveformAgreementMs = 80;
constexpr float kEnvelopeOverrideConfidenceRatio = 1.15f;
constexpr int kAutoOffsetDeadbandMs = 15;
constexpr int kAutoOffsetMaxStepMs = 20;

int clampInt(int value, int low, int high)
{
    return std::clamp(value, low, high);
}

qint64 absoluteFrequencyDeltaHz(qint64 lhs, qint64 rhs)
{
    return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

int nextPowerOfTwo(int value)
{
    int n = 1;
    while (n < value) {
        n <<= 1;
    }
    return n;
}

int overlapSamplesForOffset(int flexSize, int kiwiSize, int offsetSamples)
{
    const int flexStart = offsetSamples >= 0 ? 0 : -offsetSamples;
    const int kiwiStart = offsetSamples >= 0 ? offsetSamples : 0;
    return std::min(flexSize - flexStart, kiwiSize - kiwiStart);
}

double signalEnergy(const QVector<float>& signal)
{
    double energy = 0.0;
    for (float sample : signal) {
        energy += static_cast<double>(sample) * static_cast<double>(sample);
    }
    return energy;
}

struct FeatureVector {
    QVector<float> samples;
    int sampleRateHz{1};
};

QVector<float> normalizedCopy(const QVector<float>& input)
{
    QVector<float> out = input;
    if (out.isEmpty()) {
        return out;
    }

    double mean = 0.0;
    for (float sample : out) {
        mean += sample;
    }
    mean /= static_cast<double>(out.size());

    double energy = 0.0;
    for (float& sample : out) {
        sample = static_cast<float>(sample - mean);
        energy += static_cast<double>(sample) * static_cast<double>(sample);
    }

    if (energy <= kEpsilon) {
        out.fill(0.0f);
        return out;
    }

    const float scale = static_cast<float>(1.0 / std::sqrt(energy));
    for (float& sample : out) {
        sample *= scale;
    }
    return out;
}

float normalizedCorrelationAt(const QVector<float>& flex,
                              const QVector<float>& kiwi,
                              int offsetSamples,
                              int minOverlapSamples)
{
    const int flexSize = flex.size();
    const int kiwiSize = kiwi.size();
    if (flexSize <= 0 || kiwiSize <= 0) {
        return 0.0f;
    }

    const int flexStart = offsetSamples >= 0 ? 0 : -offsetSamples;
    const int kiwiStart = offsetSamples >= 0 ? offsetSamples : 0;
    const int overlap = std::min(flexSize - flexStart, kiwiSize - kiwiStart);
    if (overlap < minOverlapSamples) {
        return 0.0f;
    }

    double dot = 0.0;
    double flexEnergy = 0.0;
    double kiwiEnergy = 0.0;
    for (int i = 0; i < overlap; ++i) {
        const float f = flex[flexStart + i];
        const float k = kiwi[kiwiStart + i];
        dot += static_cast<double>(f) * static_cast<double>(k);
        flexEnergy += static_cast<double>(f) * static_cast<double>(f);
        kiwiEnergy += static_cast<double>(k) * static_cast<double>(k);
    }

    const double denom = std::sqrt(flexEnergy * kiwiEnergy);
    if (denom <= kEpsilon) {
        return 0.0f;
    }
    return static_cast<float>(dot / denom);
}

void copyWindowedSignal(const QVector<float>& input,
                        std::vector<std::complex<double>>& output)
{
    if (input.isEmpty()) {
        return;
    }

    double mean = 0.0;
    for (float sample : input) {
        mean += static_cast<double>(sample);
    }
    mean /= static_cast<double>(input.size());

    const double denom =
        static_cast<double>(std::max<qsizetype>(1, input.size() - 1));
    for (int i = 0; i < input.size(); ++i) {
        const double window =
            0.5 - 0.5 * std::cos((2.0 * kPi * static_cast<double>(i)) / denom);
        output[static_cast<size_t>(i)] =
            std::complex<double>((static_cast<double>(input[i]) - mean) * window,
                                 0.0);
    }
}

FeatureVector envelopeFeatures(const QVector<float>& input, int sampleRateHz)
{
    FeatureVector result;
    const int frameSamples =
        std::max(1, (sampleRateHz * kEnvelopeFrameMs) / 1000);
    result.sampleRateHz = std::max(1, sampleRateHz / frameSamples);
    if (input.size() < frameSamples) {
        return result;
    }

    result.samples.reserve(input.size() / frameSamples);
    for (int start = 0; start + frameSamples <= input.size();
         start += frameSamples) {
        double energy = 0.0;
        for (int i = 0; i < frameSamples; ++i) {
            const float sample = input[start + i];
            energy += static_cast<double>(sample)
                      * static_cast<double>(sample);
        }
        const double rms =
            std::sqrt(energy / static_cast<double>(frameSamples));
        result.samples.append(
            static_cast<float>(std::log1p(16.0 * rms)));
    }

    return result;
}

ReceiveAudioDelayEstimate estimateByNormalizedScan(
    const QVector<float>& flex,
    const QVector<float>& kiwi,
    int sampleRateHz,
    int maxOffsetMs,
    int minOverlapMs,
    float minPeakCorrelation,
    float minConfidence)
{
    ReceiveAudioDelayEstimate result;
    if (flex.isEmpty() || kiwi.isEmpty()) {
        return result;
    }
    if (signalEnergy(flex) <= kEpsilon || signalEnergy(kiwi) <= kEpsilon) {
        return result;
    }

    const QVector<float> normalizedFlex = normalizedCopy(flex);
    const QVector<float> normalizedKiwi = normalizedCopy(kiwi);
    const int maxOffsetSamples =
        (maxOffsetMs * sampleRateHz) / 1000;
    const int minOverlapSamples =
        std::max(1, (minOverlapMs * sampleRateHz) / 1000);
    const int guardSamples =
        std::max(1, (sampleRateHz * 20) / 1000);

    double bestScore = -std::numeric_limits<double>::infinity();
    double scoreSum = 0.0;
    int scoreCount = 0;
    int bestOffsetSamples = 0;
    std::vector<double> scores;
    scores.reserve(static_cast<size_t>(maxOffsetSamples * 2 + 1));
    for (int offset = -maxOffsetSamples; offset <= maxOffsetSamples; ++offset) {
        if (overlapSamplesForOffset(flex.size(), kiwi.size(), offset)
            < minOverlapSamples) {
            continue;
        }

        const double score = std::abs(
            normalizedCorrelationAt(normalizedFlex, normalizedKiwi,
                                    offset, minOverlapSamples));
        scores.push_back(score);
        scoreSum += score;
        ++scoreCount;
        if (score > bestScore) {
            bestScore = score;
            bestOffsetSamples = offset;
        }
    }

    if (!std::isfinite(bestScore) || scoreCount <= 0) {
        return result;
    }

    double secondScore = 0.0;
    for (int offset = -maxOffsetSamples; offset <= maxOffsetSamples; ++offset) {
        if (std::abs(offset - bestOffsetSamples) <= guardSamples
            || overlapSamplesForOffset(flex.size(), kiwi.size(), offset)
                   < minOverlapSamples) {
            continue;
        }
        secondScore = std::max(
            secondScore,
            static_cast<double>(std::abs(
                normalizedCorrelationAt(normalizedFlex, normalizedKiwi,
                                        offset, minOverlapSamples))));
    }

    std::sort(scores.begin(), scores.end());
    const double backgroundScore =
        scores.empty()
            ? 0.0
            : scores[static_cast<size_t>(
                std::clamp<int>(
                    static_cast<int>(std::lround(
                        static_cast<double>(scores.size() - 1) * 0.95)),
                    0,
                    static_cast<int>(scores.size() - 1)))];
    const double meanScore = scoreSum / static_cast<double>(scoreCount);
    const double peakLift =
        bestScore > kEpsilon
            ? std::clamp((bestScore - meanScore) / bestScore, 0.0, 1.0)
            : 0.0;
    const double backgroundSeparation =
        bestScore > kEpsilon
            ? std::clamp((bestScore - backgroundScore) / bestScore, 0.0, 1.0)
            : 0.0;
    const double sideLobeSeparation =
        bestScore > kEpsilon
            ? std::clamp((bestScore - secondScore) / bestScore, 0.0, 1.0)
            : 0.0;
    const float confidence =
        std::clamp(static_cast<float>(
                       bestScore
                       * std::max(backgroundSeparation,
                                  0.5 * sideLobeSeparation)
                       * (0.65 + 0.35 * peakLift)),
                   0.0f, 1.0f);

    result.offsetMs =
        static_cast<int>(std::lround(
            static_cast<double>(bestOffsetSamples) * 1000.0
            / static_cast<double>(sampleRateHz)));
    result.peakCorrelation = std::clamp(
        static_cast<float>(bestScore), 0.0f, 1.0f);
    result.confidence = confidence;
    result.valid = result.peakCorrelation >= minPeakCorrelation
                   && confidence >= minConfidence;
    return result;
}

} // namespace

QString receivePresentationVisualQueueKey(ReceivePresentationSource source,
                                          ReceivePresentationSurface surface,
                                          const QString& sourceId)
{
    const QString sourceText =
        source == ReceivePresentationSource::Flex
            ? QStringLiteral("flex")
            : QStringLiteral("kiwi");
    QString surfaceText;
    switch (surface) {
    case ReceivePresentationSurface::Audio:
        surfaceText = QStringLiteral("audio");
        break;
    case ReceivePresentationSurface::Waterfall:
        surfaceText = QStringLiteral("waterfall");
        break;
    case ReceivePresentationSurface::Spectrum:
        surfaceText = QStringLiteral("spectrum");
        break;
    case ReceivePresentationSurface::Meter:
        surfaceText = QStringLiteral("meter");
        break;
    case ReceivePresentationSurface::Analyzer:
        surfaceText = QStringLiteral("analyzer");
        break;
    case ReceivePresentationSurface::Decoder:
        surfaceText = QStringLiteral("decoder");
        break;
    case ReceivePresentationSurface::Recording:
        surfaceText = QStringLiteral("recording");
        break;
    }

    const QString trimmedSourceId = sourceId.trimmed();
    if (trimmedSourceId.isEmpty()) {
        return sourceText + QLatin1Char(':') + surfaceText;
    }
    return sourceText + QLatin1Char(':') + surfaceText
           + QLatin1Char(':') + trimmedSourceId;
}

int receivePresentationExternalKiwiDelayMs(
    const QString& sourceId,
    const QString& delaySourceId,
    int kiwiDelayMs)
{
    const int delayMs = std::max(0, kiwiDelayMs);
    const QString id = sourceId.trimmed();
    const QString delayId = delaySourceId.trimmed();
    if (delayId.isEmpty()) {
        return delayMs;
    }
    if (id.isEmpty() || id != delayId) {
        return 0;
    }
    return delayMs;
}

bool receivePresentationShouldPrebufferAfterDelayChange(int previousDelayMs,
                                                        int nextDelayMs,
                                                        bool sourceAudible,
                                                        bool hasQueuedAudio)
{
    return nextDelayMs > previousDelayMs && sourceAudible && !hasQueuedAudio;
}

ReceiveSyncAudioPairSelection selectReceiveSyncAudioPair(
    const QVector<ReceiveSyncAudioPairEndpoint>& flexCandidates,
    const QVector<ReceiveSyncAudioPairEndpoint>& kiwiCandidates,
    qint64 frequencyToleranceHz)
{
    ReceiveSyncAudioPairSelection selection;
    selection.audibleFlexCount = flexCandidates.size();
    selection.audibleKiwiCount = kiwiCandidates.size();

    if (flexCandidates.isEmpty()) {
        selection.reason = QStringLiteral("noAudibleFlex");
        return selection;
    }
    if (flexCandidates.size() > 1) {
        selection.state = ReceiveSyncAudioPairSelection::State::Ambiguous;
        selection.reason = QStringLiteral("multipleAudibleFlex");
        return selection;
    }
    if (kiwiCandidates.isEmpty()) {
        selection.reason = QStringLiteral("noAudibleKiwi");
        return selection;
    }

    const ReceiveSyncAudioPairEndpoint flex = flexCandidates.constFirst();
    QVector<ReceiveSyncAudioPairEndpoint> matches;
    for (const ReceiveSyncAudioPairEndpoint& kiwi : kiwiCandidates) {
        if (absoluteFrequencyDeltaHz(flex.frequencyHz, kiwi.frequencyHz)
            <= frequencyToleranceHz) {
            matches.append(kiwi);
        }
    }

    selection.matchingPairCount = matches.size();
    if (matches.isEmpty()) {
        selection.reason = QStringLiteral("noFrequencyMatch");
        return selection;
    }
    if (matches.size() > 1) {
        selection.state = ReceiveSyncAudioPairSelection::State::Ambiguous;
        selection.reason = QStringLiteral("multipleFrequencyMatches");
        return selection;
    }

    const ReceiveSyncAudioPairEndpoint kiwi = matches.constFirst();
    selection.state = ReceiveSyncAudioPairSelection::State::Usable;
    selection.kiwiProfileId = kiwi.kiwiProfileId;
    selection.flexSliceId = flex.sliceId;
    selection.kiwiSliceId = kiwi.sliceId;
    selection.frequencyHz = flex.frequencyHz;
    selection.reason = QStringLiteral("matchedFrequency");
    return selection;
}

void ReceivePresentationSync::setSettings(
    const ReceivePresentationSettings& settings)
{
    m_settings = settings;
    m_settings.baseLatencyMs =
        clampInt(m_settings.baseLatencyMs, kMinBaseLatencyMs, kMaxBaseLatencyMs);
    m_settings.maxOffsetMs =
        clampInt(std::abs(m_settings.maxOffsetMs), 0, kMaxOffsetMs);
    m_settings.manualOffsetMs =
        clampedOffset(m_settings.manualOffsetMs, m_settings.maxOffsetMs);
    if (m_settings.autoEstimate.valid) {
        m_settings.autoEstimate.offsetMs =
            clampedOffset(m_settings.autoEstimate.offsetMs,
                          m_settings.maxOffsetMs);
        m_settings.autoEstimate.confidence =
            std::clamp(m_settings.autoEstimate.confidence, 0.0f, 1.0f);
    }
    m_settings.autoLockConfidence =
        std::clamp(m_settings.autoLockConfidence, 0.0f, 1.0f);
}

void ReceivePresentationSync::setEnabled(bool enabled)
{
    m_settings.enabled = enabled;
}

void ReceivePresentationSync::setMode(ReceiveSyncMode mode)
{
    m_settings.mode = mode;
}

void ReceivePresentationSync::setManualOffsetMs(int offsetMs)
{
    m_settings.manualOffsetMs = clampedOffset(offsetMs, m_settings.maxOffsetMs);
}

void ReceivePresentationSync::setBaseLatencyMs(int latencyMs)
{
    m_settings.baseLatencyMs =
        clampInt(latencyMs, kMinBaseLatencyMs, kMaxBaseLatencyMs);
}

void ReceivePresentationSync::setMaxOffsetMs(int maxOffsetMs)
{
    m_settings.maxOffsetMs =
        clampInt(std::abs(maxOffsetMs), 0, kMaxOffsetMs);
    m_settings.manualOffsetMs =
        clampedOffset(m_settings.manualOffsetMs, m_settings.maxOffsetMs);
    if (m_settings.autoEstimate.valid) {
        m_settings.autoEstimate.offsetMs =
            clampedOffset(m_settings.autoEstimate.offsetMs,
                          m_settings.maxOffsetMs);
    }
}

void ReceivePresentationSync::setAutoEstimate(const ReceiveSyncEstimate& estimate)
{
    m_settings.autoEstimate = estimate;
    m_settings.autoEstimate.offsetMs =
        clampedOffset(m_settings.autoEstimate.offsetMs, m_settings.maxOffsetMs);
    m_settings.autoEstimate.confidence =
        std::clamp(m_settings.autoEstimate.confidence, 0.0f, 1.0f);
}

void ReceivePresentationSync::clearAutoEstimate()
{
    m_settings.autoEstimate = {};
}

ReceiveDelayBreakdown ReceivePresentationSync::delayBreakdown() const
{
    ReceiveDelayBreakdown result;
    if (!m_settings.enabled) {
        return result;
    }

    result.effectiveOffsetMs = effectiveOffsetMs();
    result.flexDelayMs = m_settings.baseLatencyMs
                         + std::max(0, result.effectiveOffsetMs);
    result.kiwiDelayMs = m_settings.baseLatencyMs
                         + std::max(0, -result.effectiveOffsetMs);

    if (m_settings.mode == ReceiveSyncMode::Manual) {
        result.status = ReceiveSyncStatus::Manual;
    } else if (!m_settings.autoEstimate.valid) {
        result.status = ReceiveSyncStatus::Searching;
    } else if (m_settings.autoEstimate.held) {
        result.status = ReceiveSyncStatus::Holding;
    } else if (m_settings.autoEstimate.confidence
               >= m_settings.autoLockConfidence) {
        result.status = ReceiveSyncStatus::Locked;
    } else {
        result.status = ReceiveSyncStatus::LowConfidence;
    }

    return result;
}

int ReceivePresentationSync::delayMs(ReceivePresentationSource source,
                                     ReceivePresentationSurface surface) const
{
    switch (surface) {
    case ReceivePresentationSurface::Audio:
    case ReceivePresentationSurface::Waterfall:
    case ReceivePresentationSurface::Spectrum:
    case ReceivePresentationSurface::Meter:
        break;
    case ReceivePresentationSurface::Analyzer:
    case ReceivePresentationSurface::Decoder:
    case ReceivePresentationSurface::Recording:
        return 0;
    }

    const ReceiveDelayBreakdown breakdown = delayBreakdown();
    switch (source) {
    case ReceivePresentationSource::Flex:
        return breakdown.flexDelayMs;
    case ReceivePresentationSource::KiwiSdr:
        return breakdown.kiwiDelayMs;
    }
    return 0;
}

qint64 ReceivePresentationSync::dueTimeMs(ReceivePresentationSource source,
                                          ReceivePresentationSurface surface,
                                          qint64 arrivalMs) const
{
    return arrivalMs + delayMs(source, surface);
}

int ReceivePresentationSync::adjustedAutoOffsetMs(int currentOffsetMs,
                                                  int candidateOffsetMs,
                                                  bool reset)
{
    if (reset) {
        return candidateOffsetMs;
    }

    const int deltaMs = candidateOffsetMs - currentOffsetMs;
    if (std::abs(deltaMs) <= kAutoOffsetDeadbandMs) {
        return currentOffsetMs;
    }

    return currentOffsetMs
           + clampInt(deltaMs, -kAutoOffsetMaxStepMs, kAutoOffsetMaxStepMs);
}

int ReceivePresentationSync::clampedOffset(int offsetMs, int maxAbsMs)
{
    const int maxOffset = clampInt(std::abs(maxAbsMs), 0, kMaxOffsetMs);
    return clampInt(offsetMs, -maxOffset, maxOffset);
}

int ReceivePresentationSync::effectiveOffsetMs() const
{
    if (m_settings.mode != ReceiveSyncMode::AutoAssist) {
        return m_settings.manualOffsetMs;
    }
    if (!m_settings.autoEstimate.valid) {
        return m_settings.manualOffsetMs;
    }
    if (!m_settings.autoEstimate.held
        && m_settings.autoEstimate.confidence < m_settings.autoLockConfidence) {
        return m_settings.manualOffsetMs;
    }
    return clampedOffset(m_settings.autoEstimate.offsetMs,
                         m_settings.maxOffsetMs);
}

ReceiveAudioDelayEstimator::ReceiveAudioDelayEstimator()
{
    setConfig(Config{});
}

ReceiveAudioDelayEstimator::ReceiveAudioDelayEstimator(Config config)
{
    setConfig(config);
}

void ReceiveAudioDelayEstimator::setConfig(const Config& config)
{
    m_config = config;
    m_config.sampleRateHz = std::max(1, m_config.sampleRateHz);
    m_config.maxOffsetMs = std::max(0, m_config.maxOffsetMs);
    m_config.minOverlapMs = std::max(1, m_config.minOverlapMs);
    m_config.minPeakCorrelation =
        std::clamp(m_config.minPeakCorrelation, 0.0f, 1.0f);
    m_config.minConfidence =
        std::clamp(m_config.minConfidence, 0.0f, 1.0f);
}

void ReceiveAudioDelayEstimator::fft(
    std::vector<std::complex<double>>& data,
    bool inverse)
{
    const int n = static_cast<int>(data.size());
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[static_cast<size_t>(i)],
                      data[static_cast<size_t>(j)]);
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        const double angle =
            (inverse ? 2.0 : -2.0) * kPi / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(angle), std::sin(angle));
        for (int i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            const int half = len / 2;
            for (int j = 0; j < half; ++j) {
                const std::complex<double> u =
                    data[static_cast<size_t>(i + j)];
                const std::complex<double> v =
                    data[static_cast<size_t>(i + j + half)] * w;
                data[static_cast<size_t>(i + j)] = u + v;
                data[static_cast<size_t>(i + j + half)] = u - v;
                w *= wlen;
            }
        }
    }

    if (inverse) {
        const double scale = 1.0 / static_cast<double>(n);
        for (std::complex<double>& value : data) {
            value *= scale;
        }
    }
}

ReceiveAudioDelayEstimate ReceiveAudioDelayEstimator::estimate(
    const QVector<float>& flexMono,
    const QVector<float>& kiwiMono) const
{
    const auto estimateAtRate =
        [](const QVector<float>& flex,
           const QVector<float>& kiwi,
           int sampleRateHz,
           int maxOffsetMs,
           int minOverlapMs,
           float minPeakCorrelation,
           float minConfidence) {
            ReceiveAudioDelayEstimate result;
            if (flex.isEmpty() || kiwi.isEmpty()) {
                return result;
            }
            if (signalEnergy(flex) <= kEpsilon || signalEnergy(kiwi) <= kEpsilon) {
                return result;
            }

            const int maxOffsetSamples =
                (maxOffsetMs * sampleRateHz) / 1000;
            const int minOverlapSamples =
                std::max(1, (minOverlapMs * sampleRateHz) / 1000);
            const int fftSize =
                nextPowerOfTwo(flex.size() + kiwi.size());

            std::vector<std::complex<double>> flexSpectrum(
                static_cast<size_t>(fftSize));
            std::vector<std::complex<double>> kiwiSpectrum(
                static_cast<size_t>(fftSize));
            copyWindowedSignal(flex, flexSpectrum);
            copyWindowedSignal(kiwi, kiwiSpectrum);
            fft(flexSpectrum, false);
            fft(kiwiSpectrum, false);

            for (int i = 0; i < fftSize; ++i) {
                std::complex<double> cross =
                    std::conj(flexSpectrum[static_cast<size_t>(i)])
                    * kiwiSpectrum[static_cast<size_t>(i)];
                const double mag = std::abs(cross);
                flexSpectrum[static_cast<size_t>(i)] =
                    mag > static_cast<double>(kEpsilon)
                        ? cross / mag
                        : std::complex<double>();
            }
            fft(flexSpectrum, true);

            double bestScore = -std::numeric_limits<double>::infinity();
            double scoreSum = 0.0;
            int scoreCount = 0;
            int bestOffsetSamples = 0;

            for (int offset = -maxOffsetSamples;
                 offset <= maxOffsetSamples; ++offset) {
                if (overlapSamplesForOffset(flex.size(), kiwi.size(), offset)
                    < minOverlapSamples) {
                    continue;
                }

                const int index = offset >= 0 ? offset : fftSize + offset;
                if (index < 0 || index >= fftSize) {
                    continue;
                }

                const double score =
                    std::abs(flexSpectrum[static_cast<size_t>(index)]);
                scoreSum += score;
                ++scoreCount;
                if (score > bestScore) {
                    bestScore = score;
                    bestOffsetSamples = offset;
                }
            }

            if (!std::isfinite(bestScore) || scoreCount <= 0) {
                return result;
            }

            double secondScore = 0.0;
            const int guardSamples =
                std::max(1, (sampleRateHz * 20) / 1000);
            for (int offset = -maxOffsetSamples;
                 offset <= maxOffsetSamples; ++offset) {
                if (std::abs(offset - bestOffsetSamples) <= guardSamples
                    || overlapSamplesForOffset(flex.size(), kiwi.size(), offset)
                           < minOverlapSamples) {
                    continue;
                }
                const int index = offset >= 0 ? offset : fftSize + offset;
                if (index < 0 || index >= fftSize) {
                    continue;
                }
                secondScore =
                    std::max(secondScore,
                             std::abs(flexSpectrum[static_cast<size_t>(index)]));
            }

            const QVector<float> normalizedFlex = normalizedCopy(flex);
            const QVector<float> normalizedKiwi = normalizedCopy(kiwi);
            const float refinedCorrelation = std::abs(
                normalizedCorrelationAt(normalizedFlex, normalizedKiwi,
                                        bestOffsetSamples,
                                        minOverlapSamples));
            const double meanScore = scoreSum / static_cast<double>(scoreCount);
            const double peakLift =
                bestScore > kEpsilon
                    ? std::clamp((bestScore - meanScore) / bestScore,
                                 0.0, 1.0)
                    : 0.0;
            const double sideLobeSeparation =
                bestScore > kEpsilon
                    ? std::clamp((bestScore - secondScore) / bestScore,
                                 0.0, 1.0)
                    : 0.0;
            const float confidence =
                std::clamp(static_cast<float>(
                               refinedCorrelation
                               * (0.55 + 0.45 * peakLift)
                               * (0.35 + 0.65 * sideLobeSeparation)),
                           0.0f, 1.0f);

            result.offsetMs =
                static_cast<int>(std::lround(
                    static_cast<double>(bestOffsetSamples) * 1000.0
                    / static_cast<double>(sampleRateHz)));
            result.peakCorrelation =
                std::clamp(refinedCorrelation, 0.0f, 1.0f);
            result.confidence = confidence;
            result.valid = result.peakCorrelation >= minPeakCorrelation
                           && confidence >= minConfidence;
            return result;
        };

    ReceiveAudioDelayEstimate result;
    if (flexMono.isEmpty() || kiwiMono.isEmpty()) {
        return result;
    }
    if (signalEnergy(flexMono) <= kEpsilon || signalEnergy(kiwiMono) <= kEpsilon) {
        return result;
    }

    const ReceiveAudioDelayEstimate waveform =
        estimateAtRate(flexMono, kiwiMono, m_config.sampleRateHz,
                       m_config.maxOffsetMs, m_config.minOverlapMs,
                       m_config.minPeakCorrelation, m_config.minConfidence);

    const FeatureVector flexEnvelope =
        envelopeFeatures(flexMono, m_config.sampleRateHz);
    const FeatureVector kiwiEnvelope =
        envelopeFeatures(kiwiMono, m_config.sampleRateHz);
    const ReceiveAudioDelayEstimate envelopePhat =
        estimateAtRate(flexEnvelope.samples, kiwiEnvelope.samples,
                       flexEnvelope.sampleRateHz, m_config.maxOffsetMs,
                       m_config.minOverlapMs,
                       std::min(m_config.minPeakCorrelation, 0.12f),
                       std::min(m_config.minConfidence, 0.08f));
    const ReceiveAudioDelayEstimate envelopeScan =
        estimateByNormalizedScan(flexEnvelope.samples, kiwiEnvelope.samples,
                                 flexEnvelope.sampleRateHz,
                                 m_config.maxOffsetMs,
                                 m_config.minOverlapMs,
                                 std::min(m_config.minPeakCorrelation, 0.12f),
                                 std::min(m_config.minConfidence, 0.08f));
    ReceiveAudioDelayEstimate envelope = envelopePhat;
    if (envelopeScan.valid) {
        if (!envelope.valid) {
            envelope = envelopeScan;
        } else if (std::abs(envelopeScan.offsetMs - envelope.offsetMs)
                   <= kEnvelopeWaveformAgreementMs) {
            envelope.confidence =
                std::max(envelope.confidence, envelopeScan.confidence);
            envelope.peakCorrelation =
                std::max(envelope.peakCorrelation,
                         envelopeScan.peakCorrelation);
        } else if (envelopeScan.confidence
                   >= envelope.confidence * kEnvelopeOverrideConfidenceRatio) {
            envelope = envelopeScan;
        }
    }

    if (envelope.valid && !waveform.valid) {
        return envelope;
    }
    if (envelope.valid && waveform.valid) {
        const bool estimatesAgree =
            std::abs(envelope.offsetMs - waveform.offsetMs)
            <= kEnvelopeWaveformAgreementMs;
        if (estimatesAgree) {
            return waveform;
        }
        if (envelope.confidence
            >= waveform.confidence * kEnvelopeOverrideConfidenceRatio) {
            return envelope;
        }
    }
    return waveform;
}

} // namespace AetherSDR
