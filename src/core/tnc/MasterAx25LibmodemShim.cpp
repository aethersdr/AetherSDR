#include "core/tnc/MasterAx25LibmodemShim.h"

#include "core/LogManager.h"
#include "core/tnc/Ax25FrameFormatter.h"

#include "bitstream.h"
#include "demodulator.h"

#include <QDateTime>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace MasterSDR {

namespace lm = mastersdr_libmodem_core;

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
constexpr int kDuplicateSuppressSeconds = 2;
constexpr int kTxPreambleFlags = 80;
constexpr int kTxPostambleFlags = 8;
constexpr int kTxVitaPacketFrames = 128;
constexpr double kTxAfskAmplitude = 0.35;
// Phase diversity compensates for 300 baud HF timing drift until the shim grows
// a proper packet-synchronous timing loop. Phase 1 is retained because captures
// show it recovers bursts missed by the alternate 4-sample-spaced bank.
constexpr std::array<int, 21> kHf300DecodePhaseOffsets = {
    1,
    3, 7, 11, 15, 19, 23, 27, 31, 35, 39,
    43, 47, 51, 55, 59, 63, 67, 71, 75, 79
};

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

QByteArray frameSignature(const Ax25DecodedFrame& frame)
{
    QByteArray signature;
    signature += frame.source.toUtf8();
    signature += '\0';
    signature += frame.destination.toUtf8();
    signature += '\0';
    signature += frame.path.join(QLatin1Char(',')).toUtf8();
    signature += '\0';
    signature += static_cast<char>(frame.control);
    signature += static_cast<char>(frame.pid);
    signature += '\0';
    signature += frame.payload;
    return signature;
}

Ax25DecodedFrame toDecodedFrame(const lm::ax25::frame& frame, double quality, int phaseOffsetSamples)
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
    out.decodePhaseOffsetSamples = phaseOffsetSamples;
    return out;
}

Ax25TransmitFrame toTransmitFrame(const lm::packet& packet)
{
    Ax25TransmitFrame out;
    out.source = QString::fromStdString(packet.from);
    out.destination = QString::fromStdString(packet.to);
    for (const auto& path : packet.path)
        out.path.append(QString::fromStdString(path));
    out.payload = QByteArray(packet.data.data(), static_cast<qsizetype>(packet.data.size()));
    out.payloadText = Ax25FrameFormatter::payloadText(out.payload);
    out.payloadHex = Ax25FrameFormatter::payloadHex(out.payload);
    return out;
}

QString normalizedDefaultAddress(QString address, const QString& fallback)
{
    address = address.trimmed().toUpper();
    if (address.isEmpty())
        address = fallback;
    lm::address parsed;
    if (lm::try_parse_address(address.toStdString(), parsed))
        return QString::fromStdString(lm::to_string(parsed, true));
    return fallback;
}

std::optional<lm::packet> packetFromTransmitText(const QString& text,
                                                 const QString& defaultSource,
                                                 const QString& defaultDestination,
                                                 QString& error)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        error = QStringLiteral("enter text to transmit");
        return std::nullopt;
    }

    lm::packet packet;
    const std::string monitorText = trimmed.toStdString();
    if (trimmed.contains(QLatin1Char('>')) && trimmed.contains(QLatin1Char(':'))) {
        if (lm::try_decode_packet(monitorText, packet))
            return packet;
        error = QStringLiteral("invalid monitor syntax; use SRC>DST,path:payload");
        return std::nullopt;
    }

    const QString source = normalizedDefaultAddress(defaultSource, QStringLiteral("NOCALL"));
    const QString destination = normalizedDefaultAddress(defaultDestination, QStringLiteral("APRS"));
    packet = lm::packet(source.toStdString(),
                        destination.toStdString(),
                        {},
                        text.toStdString());
    return packet;
}

void measureStereoFloatPcm(const QByteArray& pcm, double& rmsDbfs, double& peakDbfs)
{
    const int sampleCount = pcm.size() / static_cast<int>(sizeof(float));
    if (sampleCount <= 0) {
        rmsDbfs = -120.0;
        peakDbfs = -120.0;
        return;
    }

    const auto* samples = reinterpret_cast<const float*>(pcm.constData());
    double sumSquares = 0.0;
    double peak = 0.0;
    for (int i = 0; i < sampleCount; ++i) {
        const double sample = std::isfinite(samples[i])
            ? std::clamp(static_cast<double>(samples[i]), -1.0, 1.0)
            : 0.0;
        sumSquares += sample * sample;
        peak = std::max(peak, std::abs(sample));
    }
    rmsDbfs = toDbfs(std::sqrt(sumSquares / static_cast<double>(sampleCount)));
    peakDbfs = toDbfs(peak);
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

struct MasterAx25LibmodemShim::Impl {
    Ax25DemodConfig config;
    struct DecodeLane {
        int phaseOffsetSamples{0};
        int samplesUntilStart{0};
        std::unique_ptr<lm::sinc_corr_afsk_demodulator> demod;
        lm::ax25::bitstream_state bitstreamState;
        std::vector<uint8_t> bitstreamBuffer;
        std::array<uint8_t, 1000> candidateFrameBytes{};
        double lastQuality{0.0};
    };
    struct RecentFrame {
        QByteArray signature;
        quint64 sampleIndex{0};
    };

    std::vector<DecodeLane> lanes;
    quint64 totalAudioSamplesProcessed{0};
    quint64 currentDecodeSampleIndex{0};
    std::vector<RecentFrame> recentFrames;
    quint64 totalHdlcFrameStarts{0};
    quint64 totalHdlcFrameCandidates{0};
    quint64 totalPlausibleAx25Candidates{0};
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
        configure(config);
    }

    void configure(const Ax25DemodConfig& next)
    {
        config = next;
        lanes.clear();

        const double mark = config.polarity == Ax25TonePolarity::Inverted
            ? config.spaceHz
            : config.markHz;
        const double space = config.polarity == Ax25TonePolarity::Inverted
            ? config.markHz
            : config.spaceHz;

        const double pllAlpha = config.profile == Ax25ModemProfile::Hf300 ? 0.0 : 0.015;

        auto addLane = [&](int phaseOffsetSamples) {
            auto& lane = lanes.emplace_back();
            lane.phaseOffsetSamples = phaseOffsetSamples;
            lane.samplesUntilStart = phaseOffsetSamples;
            lane.demod = std::make_unique<lm::sinc_corr_afsk_demodulator>(
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
            lane.bitstreamBuffer.reserve(8192);
            lane.bitstreamBuffer.resize(8192);
        };

        if (config.profile == Ax25ModemProfile::Hf300) {
            for (int phaseOffset : kHf300DecodePhaseOffsets)
                addLane(phaseOffset);
        } else {
            addLane(0);
        }
        resetDecoderState(true, true);
    }

    void resetDecoderState(bool clearCounters, bool clearDiagnostics)
    {
        for (auto& lane : lanes) {
            if (lane.demod)
                lane.demod->reset();
            resetLaneBitstream(lane);
            lane.lastQuality = 0.0;
            lane.samplesUntilStart = lane.phaseOffsetSamples;
        }

        if (clearCounters) {
            totalAudioSamplesProcessed = 0;
            currentDecodeSampleIndex = 0;
            recentFrames.clear();
            totalHdlcFrameStarts = 0;
            totalHdlcFrameCandidates = 0;
            totalPlausibleAx25Candidates = 0;
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

    void resetLaneBitstream(DecodeLane& lane)
    {
        lane.bitstreamState.reset();
        lane.bitstreamState.max_frame_bits = 4096;
        lane.bitstreamBuffer.clear();
        lane.bitstreamBuffer.resize(8192);
    }

    void resetBitstreamStates()
    {
        for (auto& lane : lanes)
            resetLaneBitstream(lane);
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
                resetBitstreamStates();
                if (diagnosticsLoggingEnabled) {
                    qCDebug(lcAx25).nospace()
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
                qCDebug(lcAx25).nospace()
                    << "receive gate closed: rms="
                    << QString::number(receiveGateRmsDbfs, 'f', 1) << "dBFS floor="
                    << QString::number(receiveGateFloorDbfs, 'f', 1) << "dBFS";
            }
        }
    }

    bool candidateHasAx25Structure(const DecodeLane& lane,
                                   size_t frameBytesSize,
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
            lane.candidateFrameBytes.data(),
            lane.candidateFrameBytes.data() + frameBytesSize - 2,
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

    bool recordReject(const DecodeLane& lane,
                      size_t frameBytesSize,
                      const std::array<uint8_t, 2>& actualFcs,
                      const std::array<uint8_t, 2>& expectedFcs)
    {
        ++totalDecodeRejected;
        lastRejectFrameBits = static_cast<int>(lane.bitstreamState.frame_size_bits);
        lastRejectFrameBytes = static_cast<int>(frameBytesSize);
        lastRejectPreviewHex = framePreviewHex(lane.candidateFrameBytes, frameBytesSize);
        lastRejectActualFcs = frameBytesSize >= 18 ? fcsToString(actualFcs) : QString();
        lastRejectExpectedFcs = frameBytesSize >= 18 ? fcsToString(expectedFcs) : QString();

        if (frameBytesSize < 18) {
            ++totalRejectTooShort;
            lastRejectReason = QStringLiteral("too-short");
            return false;
        }

        uint8_t control = 0;
        uint8_t pid = 0;
        size_t pathCount = 0;
        size_t dataLength = 0;
        const bool ax25Like = candidateHasAx25Structure(lane, frameBytesSize, control, pid, pathCount, dataLength);

        if (actualFcs != expectedFcs) {
            if (ax25Like) {
                ++totalRejectBadFcs;
                lastRejectReason = QStringLiteral("bad-fcs ctrl=%1 pid=%2 path=%3 data=%4")
                    .arg(control, 2, 16, QLatin1Char('0'))
                    .arg(pid, 2, 16, QLatin1Char('0'))
                    .arg(pathCount)
                    .arg(dataLength)
                    .toUpper();
                return true;
            } else {
                ++totalRejectMalformed;
                lastRejectReason = QStringLiteral("malformed+bad-fcs");
            }
            return false;
        }

        ++totalRejectMalformed;
        lastRejectReason = QStringLiteral("malformed");
        return false;
    }

    std::optional<Ax25DecodedFrame> processBit(DecodeLane& lane, uint8_t bit, double quality)
    {
        lane.lastQuality = 0.95 * lane.lastQuality + 0.05 * quality;
        const bool wasComplete = lane.bitstreamState.complete;
        const bool wasInFrame = lane.bitstreamState.in_frame;
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
            lane.bitstreamState,
            lane.bitstreamBuffer.begin(),
            lane.bitstreamBuffer.end(),
            lane.candidateFrameBytes,
            from,
            to,
            path.begin(),
            data.begin(),
            data.size(),
            control,
            pid,
            actualFcs,
            expectedFcs);

        if (lane.bitstreamState.in_frame && !wasInFrame)
            ++totalHdlcFrameStarts;

        if (lane.bitstreamState.complete && !wasComplete) {
            ++totalHdlcFrameCandidates;
            if (decoded) {
                ++totalPlausibleAx25Candidates;
            } else {
                if (recordReject(lane, candidateFrameBytesSize, actualFcs, expectedFcs))
                    ++totalPlausibleAx25Candidates;
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
        return toDecodedFrame(frame, lane.lastQuality, lane.phaseOffsetSamples);
    }

    bool shouldEmitFrame(const Ax25DecodedFrame& frame)
    {
        const QByteArray signature = frameSignature(frame);
        const quint64 duplicateWindowSamples = static_cast<quint64>(
            std::max(1, config.sampleRate) * kDuplicateSuppressSeconds);

        recentFrames.erase(std::remove_if(recentFrames.begin(),
                                          recentFrames.end(),
                                          [&](const RecentFrame& recent) {
                                              return currentDecodeSampleIndex >= recent.sampleIndex
                                                  && currentDecodeSampleIndex - recent.sampleIndex > duplicateWindowSamples;
                                          }),
                           recentFrames.end());

        const auto duplicate = std::find_if(recentFrames.begin(),
                                            recentFrames.end(),
                                            [&](const RecentFrame& recent) {
                                                return recent.signature == signature
                                                    && currentDecodeSampleIndex >= recent.sampleIndex
                                                    && currentDecodeSampleIndex - recent.sampleIndex <= duplicateWindowSamples;
                                            });
        if (duplicate != recentFrames.end())
            return false;

        recentFrames.push_back({ signature, currentDecodeSampleIndex });
        ++totalFramesAccepted;
        return true;
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
        diagnostics.decodeLanes = static_cast<int>(lanes.size());
        diagnostics.demodSymbols = diagnosticsWindow.demodSymbols;
        diagnostics.averageConfidence = diagnosticsWindow.demodSymbols > 0
            ? diagnosticsWindow.confidenceSum / static_cast<double>(diagnosticsWindow.demodSymbols)
            : 0.0;
        diagnostics.onesPercent = diagnosticsWindow.demodSymbols > 0
            ? 100.0 * static_cast<double>(diagnosticsWindow.oneBits)
                / static_cast<double>(diagnosticsWindow.demodSymbols)
            : 0.0;
        diagnostics.searching = true;
        diagnostics.inPreamble = false;
        diagnostics.inFrame = false;
        diagnostics.aborted = false;
        diagnostics.currentFrameBits = 0;
        diagnostics.lastFrameBits = 0;
        diagnostics.preambleFlags = 0;
        for (const auto& lane : lanes) {
            diagnostics.searching = diagnostics.searching && lane.bitstreamState.searching;
            diagnostics.inPreamble = diagnostics.inPreamble || lane.bitstreamState.in_preamble;
            diagnostics.inFrame = diagnostics.inFrame || lane.bitstreamState.in_frame;
            diagnostics.aborted = diagnostics.aborted || lane.bitstreamState.aborted;
            diagnostics.currentFrameBits = std::max(diagnostics.currentFrameBits,
                                                    static_cast<int>(lane.bitstreamState.bitstream_size));
            diagnostics.lastFrameBits = std::max(diagnostics.lastFrameBits,
                                                 static_cast<int>(lane.bitstreamState.frame_size_bits));
            diagnostics.preambleFlags = std::max(diagnostics.preambleFlags,
                                                 static_cast<int>(lane.bitstreamState.preamble_count));
        }
        diagnostics.hdlcFrameStarts = totalHdlcFrameStarts;
        diagnostics.hdlcFrameCandidates = totalHdlcFrameCandidates;
        diagnostics.plausibleAx25Candidates = totalPlausibleAx25Candidates;
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

MasterAx25LibmodemShim::MasterAx25LibmodemShim(QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
    qRegisterMetaType<MasterSDR::Ax25DecodedFrame>("MasterSDR::Ax25DecodedFrame");
    qRegisterMetaType<MasterSDR::Ax25DecoderDiagnostics>("MasterSDR::Ax25DecoderDiagnostics");
}

MasterAx25LibmodemShim::~MasterAx25LibmodemShim() = default;

Ax25DemodConfig MasterAx25LibmodemShim::config() const
{
    return m_impl->config;
}

void MasterAx25LibmodemShim::configure(const Ax25DemodConfig& config)
{
    m_impl->configure(config);
    emit statusChanged();
}

void MasterAx25LibmodemShim::reset()
{
    m_impl->resetDecoderState(true, true);
    emit statusChanged();
}

void MasterAx25LibmodemShim::setDiagnosticsLoggingEnabled(bool enabled)
{
    m_impl->diagnosticsLoggingEnabled = enabled;
}

bool MasterAx25LibmodemShim::diagnosticsLoggingEnabled() const
{
    return m_impl->diagnosticsLoggingEnabled;
}

QVector<Ax25DecodedFrame> MasterAx25LibmodemShim::processMonoFloat(const float* samples,
                                                                   int sampleCount,
                                                                   int sampleRate)
{
    QVector<Ax25DecodedFrame> frames;
    if (!samples || sampleCount <= 0 || m_impl->lanes.empty())
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
        m_impl->currentDecodeSampleIndex = m_impl->totalAudioSamplesProcessed;
        for (size_t laneIndex = 0; laneIndex < m_impl->lanes.size(); ++laneIndex) {
            auto& lane = m_impl->lanes[laneIndex];
            if (lane.samplesUntilStart > 0) {
                --lane.samplesUntilStart;
                continue;
            }

            lm::demod_result result;
            if (!lane.demod || !lane.demod->try_demodulate(sample, result))
                continue;
            if (laneIndex == 0)
                m_impl->recordDemodSymbol(result);
            if (auto decoded = m_impl->processBit(lane, result.bit, result.confidence);
                decoded && m_impl->shouldEmitFrame(*decoded)) {
                frames.append(*decoded);
            }
        }
        ++m_impl->totalAudioSamplesProcessed;
    }
    return frames;
}

QVector<Ax25DecodedFrame> MasterAx25LibmodemShim::processRecoveredBitsForTest(
    const QVector<quint8>& bits,
    double quality)
{
    QVector<Ax25DecodedFrame> frames;
    if (m_impl->lanes.empty())
        return frames;
    auto& lane = m_impl->lanes.front();
    for (quint8 bit : bits) {
        m_impl->currentDecodeSampleIndex++;
        if (auto decoded = m_impl->processBit(lane, bit, quality);
            decoded && m_impl->shouldEmitFrame(*decoded)) {
            frames.append(*decoded);
        }
    }
    return frames;
}

Ax25TransmitResult MasterAx25LibmodemShim::buildTransmitAudio(
    const QString& text,
    const QString& defaultSource,
    const QString& defaultDestination) const
{
    Ax25TransmitResult result;
    const auto cfg = m_impl->config;
    result.sampleRate = cfg.sampleRate;
    result.baud = cfg.baud;
    result.polarity = cfg.polarity;
    result.markHz = cfg.markHz;
    result.spaceHz = cfg.spaceHz;
    result.preambleFlags = kTxPreambleFlags;
    result.postambleFlags = kTxPostambleFlags;
    result.vitaPacketFrames = kTxVitaPacketFrames;

    if (cfg.sampleRate <= 0 || cfg.baud <= 0 || cfg.sampleRate % cfg.baud != 0) {
        result.error = QStringLiteral("unsupported TX sample-rate/baud combination: %1 Hz / %2 baud")
            .arg(cfg.sampleRate)
            .arg(cfg.baud);
        return result;
    }

    QString error;
    const std::optional<lm::packet> maybePacket =
        packetFromTransmitText(text, defaultSource, defaultDestination, error);
    if (!maybePacket) {
        result.error = error;
        return result;
    }
    const lm::packet& packet = *maybePacket;
    result.frame = toTransmitFrame(packet);

    const lm::ax25::frame frame = lm::ax25::to_frame(packet);
    if (!lm::ax25::validate_frame(frame)) {
        result.error = QStringLiteral("invalid AX.25 address or frame fields");
        return result;
    }

    const std::vector<uint8_t> frameBytes = lm::ax25::encode_frame(packet);
    const std::vector<uint8_t> bits = lm::ax25::encode_bitstream(
        frameBytes,
        0,
        kTxPreambleFlags,
        kTxPostambleFlags);
    result.frameBytes = static_cast<int>(frameBytes.size());
    result.bitCount = static_cast<int>(bits.size());

    const int samplesPerSymbol = cfg.sampleRate / cfg.baud;
    const int payloadFrames = static_cast<int>(bits.size()) * samplesPerSymbol;
    const int paddedFrames = ((payloadFrames + kTxVitaPacketFrames - 1) / kTxVitaPacketFrames)
        * kTxVitaPacketFrames;
    result.audioFrames = paddedFrames;
    result.durationSeconds = static_cast<double>(paddedFrames) / static_cast<double>(cfg.sampleRate);
    result.stereoFloat32Pcm.resize(paddedFrames * 2 * static_cast<int>(sizeof(float)));

    const double mark = cfg.polarity == Ax25TonePolarity::Inverted
        ? cfg.spaceHz
        : cfg.markHz;
    const double space = cfg.polarity == Ax25TonePolarity::Inverted
        ? cfg.markHz
        : cfg.spaceHz;
    double phase = 0.0;
    int frameIndex = 0;
    auto* dst = reinterpret_cast<float*>(result.stereoFloat32Pcm.data());
    for (uint8_t bit : bits) {
        const double frequency = bit ? mark : space;
        const double phaseStep = 2.0 * kPi * frequency / static_cast<double>(cfg.sampleRate);
        for (int i = 0; i < samplesPerSymbol; ++i) {
            const float sample = static_cast<float>(kTxAfskAmplitude * std::sin(phase));
            dst[frameIndex * 2] = sample;
            dst[frameIndex * 2 + 1] = sample;
            ++frameIndex;
            phase += phaseStep;
            if (phase >= 2.0 * kPi)
                phase -= 2.0 * kPi;
        }
    }
    while (frameIndex < paddedFrames) {
        dst[frameIndex * 2] = 0.0f;
        dst[frameIndex * 2 + 1] = 0.0f;
        ++frameIndex;
    }
    measureStereoFloatPcm(result.stereoFloat32Pcm, result.rmsDbfs, result.peakDbfs);
    result.ok = true;

    qCInfo(lcAx25).noquote()
        << QStringLiteral("AX.25 TX packetized SRC=%1 DST=%2 VIA=%3 payloadBytes=%4 frameBytes=%5 bits=%6 samples=%7 duration=%8s levelRms=%9dBFS levelPeak=%10dBFS baud=%11 mark=%12 space=%13 polarity=%14")
            .arg(result.frame.source,
                 result.frame.destination,
                 result.frame.path.join(QStringLiteral(",")))
            .arg(result.frame.payload.size())
            .arg(result.frameBytes)
            .arg(result.bitCount)
            .arg(result.audioFrames)
            .arg(result.durationSeconds, 0, 'f', 2)
            .arg(result.rmsDbfs, 0, 'f', 1)
            .arg(result.peakDbfs, 0, 'f', 1)
            .arg(result.baud)
            .arg(result.markHz, 0, 'f', 0)
            .arg(result.spaceHz, 0, 'f', 0)
            .arg(result.polarity == Ax25TonePolarity::Normal
                 ? QStringLiteral("Normal")
                 : QStringLiteral("Reverse"));
    return result;
}

Ax25DecoderDiagnostics MasterAx25LibmodemShim::diagnosticsSnapshot() const
{
    return m_impl->makeDiagnostics(m_impl->config.sampleRate);
}

QString MasterAx25LibmodemShim::demodDescription() const
{
    const auto cfg = m_impl->config;
    return QStringLiteral("%1: %2 Hz, %3 bps, mark %4 Hz, space %5 Hz, %6, %7 lane%8")
        .arg(ax25ModemProfileName(cfg.profile))
        .arg(cfg.sampleRate)
        .arg(cfg.baud)
        .arg(cfg.markHz, 0, 'f', 0)
        .arg(cfg.spaceHz, 0, 'f', 0)
        .arg(cfg.polarity == Ax25TonePolarity::Normal
             ? QStringLiteral("Normal")
             : QStringLiteral("Inverted"))
        .arg(m_impl->lanes.size())
        .arg(m_impl->lanes.size() == 1 ? QString() : QStringLiteral("s"));
}

void MasterAx25LibmodemShim::feedAudio(const QByteArray& monoFloat32Pcm, int sampleRate)
{
    const int sampleCount = monoFloat32Pcm.size() / static_cast<int>(sizeof(float));
    const auto* samples = reinterpret_cast<const float*>(monoFloat32Pcm.constData());
    const QVector<Ax25DecodedFrame> frames = processMonoFloat(samples, sampleCount, sampleRate);
    for (const auto& frame : frames) {
        qCDebug(lcAx25).noquote()
            << QStringLiteral("decoded AX.25 frame SRC=%1 DST=%2 VIA=%3 UI=%4 pid=%5 phase=%6 payload=%7")
                .arg(frame.source,
                     frame.destination,
                     frame.path.join(QStringLiteral(",")),
                     frame.isUiFrame ? QStringLiteral("yes") : QStringLiteral("no"),
                     QStringLiteral("%1").arg(frame.pid, 2, 16, QLatin1Char('0')).toUpper(),
                     QString::number(frame.decodePhaseOffsetSamples),
                     frame.payloadText.isEmpty() ? frame.payloadHex : frame.payloadText);
        emit frameDecoded(frame);
    }
    if (auto diagnostics = m_impl->takeDiagnosticsIfReady(sampleRate)) {
        if (m_impl->diagnosticsLoggingEnabled) {
            qCDebug(lcAx25).nospace()
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
                << " lanes=" << diagnostics->decodeLanes
                << " symbols=" << diagnostics->demodSymbols
                << " conf=" << QString::number(diagnostics->averageConfidence, 'f', 2)
                << " ones=" << QString::number(diagnostics->onesPercent, 'f', 1) << "%"
                << " state="
                << (diagnostics->inFrame ? "frame" : diagnostics->inPreamble ? "preamble" : "search")
                << " bits=" << diagnostics->currentFrameBits
                << " starts=" << diagnostics->hdlcFrameStarts
                << " hdlc=" << diagnostics->hdlcFrameCandidates
                << " ax25=" << diagnostics->plausibleAx25Candidates
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

} // namespace MasterSDR
