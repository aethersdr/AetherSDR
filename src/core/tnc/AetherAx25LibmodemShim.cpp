#include "core/tnc/AetherAx25LibmodemShim.h"

#include "core/tnc/Ax25FrameFormatter.h"

#include "bitstream.h"
#include "demodulator.h"

#include <QDateTime>
#include <QLoggingCategory>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <optional>
#include <vector>

Q_LOGGING_CATEGORY(lcAetherAx25Shim, "aether.ax25")

namespace AetherSDR {

namespace lm = aether_libmodem_core;

namespace {

double toDbfs(double value)
{
    constexpr double floor = 1.0e-6;
    return 20.0 * std::log10(std::max(value, floor));
}

constexpr double kPi = 3.14159265358979323846;
constexpr double kReceiveGateOpenRiseDb = 3.0;
constexpr double kReceiveGateCloseRiseDb = 1.0;
constexpr double kReceiveGateMinimumDbfs = -32.0;
constexpr double kReceiveGateFloorAlpha = 0.04;
constexpr double kReceiveGateCloseSeconds = 3.0;

QString fcsToString(const std::array<uint8_t, 2>& fcs)
{
    return QStringLiteral("%1%2")
        .arg(fcs[1], 2, 16, QLatin1Char('0'))
        .arg(fcs[0], 2, 16, QLatin1Char('0'))
        .toUpper();
}

template <size_t N>
QString framePreviewHex(const std::array<uint8_t, N>& bytes, size_t byteCount)
{
    const qsizetype previewBytes = static_cast<qsizetype>(std::min<size_t>(byteCount, 24));
    QByteArray preview(reinterpret_cast<const char*>(bytes.data()), previewBytes);
    return QString::fromLatin1(preview.toHex(' ')).toUpper();
}

QString addressToString(const lm::address& address)
{
    QString text = QString::fromLatin1(address.text.data(), static_cast<int>(address.text_length));
    if (address.ssid != 0)
        text.append(QStringLiteral("-%1").arg(address.ssid));
    return text;
}

Ax25DecodedFrame toDecodedFrame(const lm::ax25::frame& frame, double quality)
{
    Ax25DecodedFrame out;
    out.timestampUtc = QDateTime::currentDateTimeUtc();
    out.source = addressToString(frame.from);
    out.destination = addressToString(frame.to);
    for (size_t i = 0; i < frame.path_count; ++i)
        out.path.append(addressToString(frame.path[i]));
    out.control = frame.control[0];
    out.pid = frame.pid;
    out.payload = QByteArray(reinterpret_cast<const char*>(frame.data.data()),
                             static_cast<qsizetype>(frame.data_length));
    out.payloadText = Ax25FrameFormatter::payloadText(out.payload);
    out.payloadHex = Ax25FrameFormatter::payloadHex(out.payload);
    out.isUiFrame = (out.control == 0x03 && out.pid == 0xf0);
    out.fcsOk = true;
    out.confidenceOrQuality = quality;
    return out;
}

} // namespace

Ax25DemodConfig ax25DemodConfigForProfile(Ax25ModemProfile profile, Ax25TonePolarity polarity)
{
    Ax25DemodConfig config;
    config.profile = profile;
    config.sampleRate = 24000;
    config.polarity = polarity;

    switch (profile) {
    case Ax25ModemProfile::Hf300:
        config.baud = 300;
        config.markHz = 1600.0;
        config.spaceHz = 1800.0;
        break;
    case Ax25ModemProfile::Vhf1200:
        config.baud = 1200;
        config.markHz = 1200.0;
        config.spaceHz = 2200.0;
        break;
    }

    return config;
}

QString ax25ModemProfileName(Ax25ModemProfile profile)
{
    switch (profile) {
    case Ax25ModemProfile::Hf300:
        return QStringLiteral("300 baud HF");
    case Ax25ModemProfile::Vhf1200:
        return QStringLiteral("1200 baud VHF");
    }
    return QStringLiteral("AX.25");
}

struct AetherAx25LibmodemShim::Impl {
    Ax25DemodConfig config;
    std::unique_ptr<lm::sinc_corr_afsk_demodulator> demod;
    lm::ax25::bitstream_state bitstreamState;
    std::vector<uint8_t> bitstreamBuffer;
    std::array<uint8_t, 1000> candidateFrameBytes{};
    double lastQuality{0.0};
    quint64 totalHdlcFrameCandidates{0};
    quint64 totalFramesAccepted{0};
    quint64 totalDecodeRejected{0};
    quint64 totalRejectTooShort{0};
    quint64 totalRejectBadFcs{0};
    quint64 totalRejectMalformed{0};
    QString lastRejectReason;
    QString lastRejectPreviewHex;
    QString lastRejectActualFcs;
    QString lastRejectExpectedFcs;
    int lastRejectFrameBits{0};
    int lastRejectFrameBytes{0};
    bool receiveGateOpen{false};
    bool receiveGateFloorInitialized{false};
    int receiveGateIdleSamples{0};
    int receiveGateSampleRate{0};
    quint64 receiveGateResets{0};
    double receiveGateRmsDbfs{-120.0};
    double receiveGateFloorDbfs{-120.0};
    bool diagnosticsLoggingEnabled{false};

    struct TonePowerMeter {
        double frequencyHz{0.0};
        double coefficient{0.0};
        double q1{0.0};
        double q2{0.0};
        int sampleRate{0};

        void configure(double frequency, int rate)
        {
            if (frequencyHz == frequency && sampleRate == rate)
                return;
            frequencyHz = frequency;
            sampleRate = rate;
            if (sampleRate <= 0 || frequencyHz <= 0.0) {
                coefficient = 0.0;
                reset();
                return;
            }
            const double omega = 2.0 * kPi * frequencyHz / static_cast<double>(sampleRate);
            coefficient = 2.0 * std::cos(omega);
            reset();
        }

        void reset()
        {
            q1 = 0.0;
            q2 = 0.0;
        }

        void record(double sample)
        {
            const double q0 = sample + coefficient * q1 - q2;
            q2 = q1;
            q1 = q0;
        }

        double amplitude(int sampleCount) const
        {
            if (sampleCount <= 0 || sampleRate <= 0 || frequencyHz <= 0.0)
                return 0.0;
            const double power = std::max(0.0, q1 * q1 + q2 * q2 - coefficient * q1 * q2);
            return 2.0 * std::sqrt(power) / static_cast<double>(sampleCount);
        }
    };

    struct DiagnosticsWindow {
        int sampleRate{0};
        int audioSamples{0};
        double sumSquares{0.0};
        double peak{0.0};
        int clippedSamples{0};
        int demodSymbols{0};
        int oneBits{0};
        double confidenceSum{0.0};
        TonePowerMeter markTone;
        TonePowerMeter spaceTone;
    } diagnosticsWindow;

    Impl()
    {
        bitstreamBuffer.reserve(8192);
        configure(config);
    }

    void configure(const Ax25DemodConfig& next)
    {
        config = next;
        const double mark = config.polarity == Ax25TonePolarity::Inverted
            ? config.spaceHz
            : config.markHz;
        const double space = config.polarity == Ax25TonePolarity::Inverted
            ? config.markHz
            : config.spaceHz;

        const double pllAlpha = config.profile == Ax25ModemProfile::Hf300 ? 0.0 : 0.015;

        demod = std::make_unique<lm::sinc_corr_afsk_demodulator>(
            mark,
            space,
            config.baud,
            config.sampleRate,
            0.75,
            6.0,
            0.75,
            3.0,
            0.008,
            0.005,
            pllAlpha);
        resetDecoderState(true, true);
    }

    void resetDecoderState(bool clearCounters, bool clearDiagnostics)
    {
        if (demod)
            demod->reset();
        bitstreamState.reset();
        bitstreamState.max_frame_bits = 4096;
        bitstreamBuffer.clear();
        bitstreamBuffer.resize(8192);
        lastQuality = 0.0;

        if (clearCounters) {
            totalHdlcFrameCandidates = 0;
            totalFramesAccepted = 0;
            totalDecodeRejected = 0;
            totalRejectTooShort = 0;
            totalRejectBadFcs = 0;
            totalRejectMalformed = 0;
            lastRejectReason.clear();
            lastRejectPreviewHex.clear();
            lastRejectActualFcs.clear();
            lastRejectExpectedFcs.clear();
            lastRejectFrameBits = 0;
            lastRejectFrameBytes = 0;
            receiveGateOpen = false;
            receiveGateFloorInitialized = false;
            receiveGateIdleSamples = 0;
            receiveGateSampleRate = 0;
            receiveGateResets = 0;
            receiveGateRmsDbfs = -120.0;
            receiveGateFloorDbfs = -120.0;
        }

        if (clearDiagnostics)
            diagnosticsWindow = {};
    }

    double measureBlockRmsDbfs(const float* samples, int sampleCount) const
    {
        if (!samples || sampleCount <= 0)
            return -120.0;

        double sumSquares = 0.0;
        for (int i = 0; i < sampleCount; ++i) {
            const float sample = std::isfinite(samples[i])
                ? std::clamp(samples[i], -1.0f, 1.0f)
                : 0.0f;
            sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
        }

        return toDbfs(std::sqrt(sumSquares / static_cast<double>(sampleCount)));
    }

    void updateReceiveGate(const float* samples, int sampleCount, int sampleRate)
    {
        receiveGateRmsDbfs = measureBlockRmsDbfs(samples, sampleCount);

        if (sampleRate != receiveGateSampleRate) {
            receiveGateSampleRate = sampleRate;
            receiveGateFloorInitialized = false;
            receiveGateOpen = false;
            receiveGateIdleSamples = 0;
        }

        if (!receiveGateFloorInitialized) {
            receiveGateFloorDbfs = receiveGateRmsDbfs;
            receiveGateFloorInitialized = true;
        }

        if (!receiveGateOpen) {
            const bool packetLike = receiveGateRmsDbfs >= kReceiveGateMinimumDbfs
                && receiveGateRmsDbfs >= receiveGateFloorDbfs + kReceiveGateOpenRiseDb;

            if (packetLike) {
                receiveGateOpen = true;
                receiveGateIdleSamples = 0;
                ++receiveGateResets;
                resetDecoderState(false, false);
                if (diagnosticsLoggingEnabled) {
                    qCDebug(lcAetherAx25Shim).nospace()
                        << "receive gate opened: rms="
                        << QString::number(receiveGateRmsDbfs, 'f', 1) << "dBFS floor="
                        << QString::number(receiveGateFloorDbfs, 'f', 1) << "dBFS resets="
                        << receiveGateResets;
                }
                return;
            }

            receiveGateFloorDbfs =
                (1.0 - kReceiveGateFloorAlpha) * receiveGateFloorDbfs
                + kReceiveGateFloorAlpha * receiveGateRmsDbfs;
            return;
        }

        const bool quiet = receiveGateRmsDbfs < kReceiveGateMinimumDbfs
            || receiveGateRmsDbfs <= receiveGateFloorDbfs + kReceiveGateCloseRiseDb;
        if (quiet) {
            receiveGateIdleSamples += sampleCount;
        } else {
            receiveGateIdleSamples = 0;
        }

        const int closeSamples = static_cast<int>(
            kReceiveGateCloseSeconds * static_cast<double>(std::max(1, sampleRate)));
        if (receiveGateIdleSamples >= closeSamples) {
            receiveGateOpen = false;
            receiveGateIdleSamples = 0;
            receiveGateFloorDbfs = receiveGateRmsDbfs;
            resetDecoderState(false, false);
            if (diagnosticsLoggingEnabled) {
                qCDebug(lcAetherAx25Shim).nospace()
                    << "receive gate closed: rms="
                    << QString::number(receiveGateRmsDbfs, 'f', 1) << "dBFS floor="
                    << QString::number(receiveGateFloorDbfs, 'f', 1) << "dBFS";
            }
        }
    }

    bool candidateHasAx25Structure(size_t frameBytesSize,
                                   uint8_t& control,
                                   uint8_t& pid,
                                   size_t& pathCount,
                                   size_t& dataLength) const
    {
        if (frameBytesSize < 2)
            return false;

        lm::address from;
        lm::address to;
        std::array<lm::address, 8> path = {};
        std::array<uint8_t, 256> data = {};
        control = 0;
        pid = 0;

        auto [pathOut, dataOut, parsed] = lm::ax25::try_decode_frame_no_fcs(
            candidateFrameBytes.data(),
            candidateFrameBytes.data() + frameBytesSize - 2,
            from,
            to,
            path.begin(),
            data.begin(),
            data.size(),
            control,
            pid);
        if (!parsed)
            return false;

        pathCount = static_cast<size_t>(std::distance(path.begin(), pathOut));
        dataLength = static_cast<size_t>(std::distance(data.begin(), dataOut));

        const std::array<uint8_t, 2> acceptedFcs = { 0, 0 };
        return lm::ax25::validate_frame(
            from,
            to,
            path.begin(),
            pathOut,
            data.begin(),
            dataOut,
            control,
            pid,
            acceptedFcs,
            acceptedFcs);
    }

    void recordReject(size_t frameBytesSize,
                      const std::array<uint8_t, 2>& actualFcs,
                      const std::array<uint8_t, 2>& expectedFcs)
    {
        ++totalDecodeRejected;
        lastRejectFrameBits = static_cast<int>(bitstreamState.frame_size_bits);
        lastRejectFrameBytes = static_cast<int>(frameBytesSize);
        lastRejectPreviewHex = framePreviewHex(candidateFrameBytes, frameBytesSize);
        lastRejectActualFcs = frameBytesSize >= 18 ? fcsToString(actualFcs) : QString();
        lastRejectExpectedFcs = frameBytesSize >= 18 ? fcsToString(expectedFcs) : QString();

        if (frameBytesSize < 18) {
            ++totalRejectTooShort;
            lastRejectReason = QStringLiteral("too-short");
            return;
        }

        uint8_t control = 0;
        uint8_t pid = 0;
        size_t pathCount = 0;
        size_t dataLength = 0;
        const bool ax25Like = candidateHasAx25Structure(frameBytesSize, control, pid, pathCount, dataLength);

        if (actualFcs != expectedFcs) {
            if (ax25Like) {
                ++totalRejectBadFcs;
                lastRejectReason = QStringLiteral("bad-fcs ctrl=%1 pid=%2 path=%3 data=%4")
                    .arg(control, 2, 16, QLatin1Char('0'))
                    .arg(pid, 2, 16, QLatin1Char('0'))
                    .arg(pathCount)
                    .arg(dataLength)
                    .toUpper();
            } else {
                ++totalRejectMalformed;
                lastRejectReason = QStringLiteral("malformed+bad-fcs");
            }
            return;
        }

        ++totalRejectMalformed;
        lastRejectReason = QStringLiteral("malformed");
    }

    std::optional<Ax25DecodedFrame> processBit(uint8_t bit, double quality)
    {
        lastQuality = 0.95 * lastQuality + 0.05 * quality;
        const bool wasComplete = bitstreamState.complete;
        lm::address from;
        lm::address to;
        std::array<lm::address, 8> path = {};
        std::array<uint8_t, 256> data = {};
        uint8_t control = 0;
        uint8_t pid = 0;
        std::array<uint8_t, 2> actualFcs = {};
        std::array<uint8_t, 2> expectedFcs = {};

        auto [candidateFrameBytesSize, pathOut, dataOut, decoded] = lm::ax25::try_decode_bitstream(
            bit ? 1 : 0,
            bitstreamState,
            bitstreamBuffer.begin(),
            bitstreamBuffer.end(),
            candidateFrameBytes,
            from,
            to,
            path.begin(),
            data.begin(),
            data.size(),
            control,
            pid,
            actualFcs,
            expectedFcs);

        if (bitstreamState.complete && !wasComplete) {
            ++totalHdlcFrameCandidates;
            if (decoded) {
                ++totalFramesAccepted;
            } else {
                recordReject(candidateFrameBytesSize, actualFcs, expectedFcs);
            }
        }
        if (!decoded)
            return std::nullopt;

        lm::ax25::frame frame;
        frame.from = from;
        frame.to = to;
        frame.path_count = static_cast<size_t>(std::distance(path.begin(), pathOut));
        std::copy_n(path.begin(), frame.path_count, frame.path.begin());
        frame.data_length = static_cast<size_t>(std::distance(data.begin(), dataOut));
        std::copy_n(data.begin(), frame.data_length, frame.data.begin());
        frame.control[0] = control;
        frame.pid = pid;
        frame.crc = actualFcs;
        return toDecodedFrame(frame, lastQuality);
    }

    void recordAudioSample(float sample, int sampleRate)
    {
        if (diagnosticsWindow.audioSamples == 0) {
            diagnosticsWindow.markTone.configure(config.markHz, sampleRate);
            diagnosticsWindow.spaceTone.configure(config.spaceHz, sampleRate);
        }
        diagnosticsWindow.sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
        diagnosticsWindow.peak = std::max(diagnosticsWindow.peak, std::abs(static_cast<double>(sample)));
        if (std::abs(sample) >= 0.98f)
            ++diagnosticsWindow.clippedSamples;
        diagnosticsWindow.markTone.record(sample);
        diagnosticsWindow.spaceTone.record(sample);
        ++diagnosticsWindow.audioSamples;
    }

    void recordDemodSymbol(const lm::demod_result& result)
    {
        ++diagnosticsWindow.demodSymbols;
        diagnosticsWindow.oneBits += result.bit ? 1 : 0;
        diagnosticsWindow.confidenceSum += result.confidence;
    }

    Ax25DecoderDiagnostics makeDiagnostics(int sampleRate) const
    {
        Ax25DecoderDiagnostics diagnostics;
        diagnostics.sampleRate = sampleRate;
        diagnostics.audioSamples = diagnosticsWindow.audioSamples;

        if (diagnosticsWindow.audioSamples > 0) {
            const double rms = std::sqrt(diagnosticsWindow.sumSquares
                                         / static_cast<double>(diagnosticsWindow.audioSamples));
            diagnostics.rmsDbfs = toDbfs(rms);
            diagnostics.peakDbfs = toDbfs(diagnosticsWindow.peak);
            diagnostics.clippedPercent = 100.0 * static_cast<double>(diagnosticsWindow.clippedSamples)
                / static_cast<double>(diagnosticsWindow.audioSamples);
        }
        diagnostics.markToneHz = config.markHz;
        diagnostics.spaceToneHz = config.spaceHz;
        diagnostics.markToneDbfs = diagnosticsWindow.audioSamples > 0
            ? toDbfs(diagnosticsWindow.markTone.amplitude(diagnosticsWindow.audioSamples))
            : -120.0;
        diagnostics.spaceToneDbfs = diagnosticsWindow.audioSamples > 0
            ? toDbfs(diagnosticsWindow.spaceTone.amplitude(diagnosticsWindow.audioSamples))
            : -120.0;
        diagnostics.markMinusSpaceDb = diagnostics.markToneDbfs - diagnostics.spaceToneDbfs;
        diagnostics.receiveGateRmsDbfs = receiveGateRmsDbfs;
        diagnostics.receiveGateFloorDbfs = receiveGateFloorDbfs;
        diagnostics.receiveGateOpen = receiveGateOpen;
        diagnostics.receiveGateResets = receiveGateResets;
        diagnostics.demodSymbols = diagnosticsWindow.demodSymbols;
        diagnostics.averageConfidence = diagnosticsWindow.demodSymbols > 0
            ? diagnosticsWindow.confidenceSum / static_cast<double>(diagnosticsWindow.demodSymbols)
            : 0.0;
        diagnostics.onesPercent = diagnosticsWindow.demodSymbols > 0
            ? 100.0 * static_cast<double>(diagnosticsWindow.oneBits)
                / static_cast<double>(diagnosticsWindow.demodSymbols)
            : 0.0;
        diagnostics.searching = bitstreamState.searching;
        diagnostics.inPreamble = bitstreamState.in_preamble;
        diagnostics.inFrame = bitstreamState.in_frame;
        diagnostics.aborted = bitstreamState.aborted;
        diagnostics.currentFrameBits = static_cast<int>(bitstreamState.bitstream_size);
        diagnostics.lastFrameBits = static_cast<int>(bitstreamState.frame_size_bits);
        diagnostics.preambleFlags = static_cast<int>(bitstreamState.preamble_count);
        diagnostics.hdlcFrameCandidates = totalHdlcFrameCandidates;
        diagnostics.framesAccepted = totalFramesAccepted;
        diagnostics.decodeRejected = totalDecodeRejected;
        diagnostics.rejectTooShort = totalRejectTooShort;
        diagnostics.rejectBadFcs = totalRejectBadFcs;
        diagnostics.rejectMalformed = totalRejectMalformed;
        diagnostics.lastRejectReason = lastRejectReason;
        diagnostics.lastRejectPreviewHex = lastRejectPreviewHex;
        diagnostics.lastRejectActualFcs = lastRejectActualFcs;
        diagnostics.lastRejectExpectedFcs = lastRejectExpectedFcs;
        diagnostics.lastRejectFrameBits = lastRejectFrameBits;
        diagnostics.lastRejectFrameBytes = lastRejectFrameBytes;

        return diagnostics;
    }

    std::optional<Ax25DecoderDiagnostics> takeDiagnosticsIfReady(int sampleRate)
    {
        diagnosticsWindow.sampleRate = sampleRate;
        if (diagnosticsWindow.audioSamples < std::max(1, sampleRate))
            return std::nullopt;

        Ax25DecoderDiagnostics diagnostics = makeDiagnostics(diagnosticsWindow.sampleRate);
        diagnosticsWindow = {};
        return diagnostics;
    }
};

AetherAx25LibmodemShim::AetherAx25LibmodemShim(QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
    qRegisterMetaType<AetherSDR::Ax25DecodedFrame>("AetherSDR::Ax25DecodedFrame");
    qRegisterMetaType<AetherSDR::Ax25DecoderDiagnostics>("AetherSDR::Ax25DecoderDiagnostics");
}

AetherAx25LibmodemShim::~AetherAx25LibmodemShim() = default;

Ax25DemodConfig AetherAx25LibmodemShim::config() const
{
    return m_impl->config;
}

void AetherAx25LibmodemShim::configure(const Ax25DemodConfig& config)
{
    m_impl->configure(config);
    emit statusChanged();
}

void AetherAx25LibmodemShim::reset()
{
    m_impl->resetDecoderState(true, true);
    emit statusChanged();
}

void AetherAx25LibmodemShim::setDiagnosticsLoggingEnabled(bool enabled)
{
    m_impl->diagnosticsLoggingEnabled = enabled;
}

bool AetherAx25LibmodemShim::diagnosticsLoggingEnabled() const
{
    return m_impl->diagnosticsLoggingEnabled;
}

QVector<Ax25DecodedFrame> AetherAx25LibmodemShim::processMonoFloat(const float* samples,
                                                                   int sampleCount,
                                                                   int sampleRate)
{
    QVector<Ax25DecodedFrame> frames;
    if (!samples || sampleCount <= 0 || !m_impl->demod)
        return frames;

    if (sampleRate != m_impl->config.sampleRate) {
        // TODO: Wire an existing project resampler here if a future tap emits
        // anything other than the native 24 kHz remote_audio_rx stream.
        return frames;
    }

    m_impl->updateReceiveGate(samples, sampleCount, sampleRate);

    for (int i = 0; i < sampleCount; ++i) {
        const float sample = std::isfinite(samples[i])
            ? std::clamp(samples[i], -1.0f, 1.0f)
            : 0.0f;
        m_impl->recordAudioSample(sample, sampleRate);
        lm::demod_result result;
        if (!m_impl->demod->try_demodulate(sample, result))
            continue;
        m_impl->recordDemodSymbol(result);
        if (auto decoded = m_impl->processBit(result.bit, result.confidence)) {
            frames.append(*decoded);
        }
    }
    return frames;
}

QVector<Ax25DecodedFrame> AetherAx25LibmodemShim::processRecoveredBitsForTest(
    const QVector<quint8>& bits,
    double quality)
{
    QVector<Ax25DecodedFrame> frames;
    for (quint8 bit : bits) {
        if (auto decoded = m_impl->processBit(bit, quality))
            frames.append(*decoded);
    }
    return frames;
}

Ax25DecoderDiagnostics AetherAx25LibmodemShim::diagnosticsSnapshot() const
{
    return m_impl->makeDiagnostics(m_impl->config.sampleRate);
}

QString AetherAx25LibmodemShim::demodDescription() const
{
    const auto cfg = m_impl->config;
    return QStringLiteral("%1: %2 Hz, %3 bps, mark %4 Hz, space %5 Hz, %6")
        .arg(ax25ModemProfileName(cfg.profile))
        .arg(cfg.sampleRate)
        .arg(cfg.baud)
        .arg(cfg.markHz, 0, 'f', 0)
        .arg(cfg.spaceHz, 0, 'f', 0)
        .arg(cfg.polarity == Ax25TonePolarity::Normal
             ? QStringLiteral("Normal")
             : QStringLiteral("Inverted"));
}

void AetherAx25LibmodemShim::feedAudio(const QByteArray& monoFloat32Pcm, int sampleRate)
{
    const int sampleCount = monoFloat32Pcm.size() / static_cast<int>(sizeof(float));
    const auto* samples = reinterpret_cast<const float*>(monoFloat32Pcm.constData());
    const QVector<Ax25DecodedFrame> frames = processMonoFloat(samples, sampleCount, sampleRate);
    for (const auto& frame : frames) {
        qCDebug(lcAetherAx25Shim).noquote()
            << QStringLiteral("decoded AX.25 frame SRC=%1 DST=%2 VIA=%3 UI=%4 pid=%5 payload=%6")
                .arg(frame.source,
                     frame.destination,
                     frame.path.join(QStringLiteral(",")),
                     frame.isUiFrame ? QStringLiteral("yes") : QStringLiteral("no"),
                     QStringLiteral("%1").arg(frame.pid, 2, 16, QLatin1Char('0')).toUpper(),
                     frame.payloadText.isEmpty() ? frame.payloadHex : frame.payloadText);
        emit frameDecoded(frame);
    }
    if (auto diagnostics = m_impl->takeDiagnosticsIfReady(sampleRate)) {
        if (m_impl->diagnosticsLoggingEnabled) {
            qCDebug(lcAetherAx25Shim).nospace()
                << "sr=" << diagnostics->sampleRate
                << " rms=" << QString::number(diagnostics->rmsDbfs, 'f', 1) << "dBFS"
                << " peak=" << QString::number(diagnostics->peakDbfs, 'f', 1) << "dBFS"
                << " clip=" << QString::number(diagnostics->clippedPercent, 'f', 2) << "%"
                << " tone" << QString::number(diagnostics->markToneHz, 'f', 0)
                << "=" << QString::number(diagnostics->markToneDbfs, 'f', 1) << "dBFS"
                << " tone" << QString::number(diagnostics->spaceToneHz, 'f', 0)
                << "=" << QString::number(diagnostics->spaceToneDbfs, 'f', 1) << "dBFS"
                << " dTone=" << QString::number(diagnostics->markMinusSpaceDb, 'f', 1) << "dB"
                << " gate=" << (diagnostics->receiveGateOpen ? "open" : "idle")
                << " gateRms=" << QString::number(diagnostics->receiveGateRmsDbfs, 'f', 1) << "dBFS"
                << " gateFloor=" << QString::number(diagnostics->receiveGateFloorDbfs, 'f', 1) << "dBFS"
                << " gateResets=" << diagnostics->receiveGateResets
                << " symbols=" << diagnostics->demodSymbols
                << " conf=" << QString::number(diagnostics->averageConfidence, 'f', 2)
                << " ones=" << QString::number(diagnostics->onesPercent, 'f', 1) << "%"
                << " state="
                << (diagnostics->inFrame ? "frame" : diagnostics->inPreamble ? "preamble" : "search")
                << " bits=" << diagnostics->currentFrameBits
                << " hdlc=" << diagnostics->hdlcFrameCandidates
                << " ok=" << diagnostics->framesAccepted
                << " reject=" << diagnostics->decodeRejected
                << " short=" << diagnostics->rejectTooShort
                << " badFcs=" << diagnostics->rejectBadFcs
                << " malformed=" << diagnostics->rejectMalformed
                << " lastReject=" << diagnostics->lastRejectReason
                << " lastBytes=" << diagnostics->lastRejectFrameBytes
                << " lastBits=" << diagnostics->lastRejectFrameBits
                << " lastFcs="
                << diagnostics->lastRejectActualFcs << "/" << diagnostics->lastRejectExpectedFcs
                << " lastHead=" << diagnostics->lastRejectPreviewHex;
        }
        emit diagnosticsUpdated(*diagnostics);
    }
    if (!frames.isEmpty())
        emit statusChanged();
}

} // namespace AetherSDR
