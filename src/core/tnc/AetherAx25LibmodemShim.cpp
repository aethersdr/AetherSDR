#include "core/tnc/AetherAx25LibmodemShim.h"

#include "core/LogManager.h"
#include "core/tnc/Ax25FrameFormatter.h"

#include "bitstream.h"
#include "demodulator.h"

#include "core/tnc/HdlcCodec.h"

#include "AetherAFSKDemod.h"

#include <QDateTime>
#include <QVarLengthArray>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace AetherSDR {

namespace lm = aether_libmodem_core;

// Abstract demodulator interface — allows VHF (Direwolf-derived) and HF
// (libmodem) demod types to coexist in the same lane vector.
struct BitResult {
    quint64 sampleIndex;
    uint8_t bit;
    double  confidence;
};

struct IAfskDemod {
    virtual ~IAfskDemod() = default;
    virtual void processBlock(const float* samples, int count, quint64 sampleBase,
                               std::vector<BitResult>& out) = 0;
    virtual void reset() noexcept = 0;
};

template<typename Demod, typename Result>
struct AfskDemodWrapper : IAfskDemod {
    Demod inner;
    template<typename... Args>
    explicit AfskDemodWrapper(Args&&... args) : inner(std::forward<Args>(args)...) {}
    void processBlock(const float* samples, int count, quint64 sampleBase,
                      std::vector<BitResult>& out) override {
        for (int i = 0; i < count; ++i) {
            Result r;
            if (inner.try_demodulate(static_cast<double>(samples[i]), r))
                out.push_back({sampleBase + static_cast<quint64>(i), r.bit, r.confidence});
        }
    }
    void reset() noexcept override { inner.reset(); }
};

using LibmodemAfskDemod = AfskDemodWrapper<lm::sinc_corr_afsk_demodulator, lm::demod_result>;
using DirewolfAfskDemod = AfskDemodWrapper<AetherDemod::AetherAFSKDemod, AetherDemod::demod_result>;

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
// TX preamble (TXDELAY) is the run of leading HDLC flags that lets the far
// receiver's PLL/AGC settle before frame data. It is profile-specific: HF 300
// keeps its long preamble, while VHF 1200 uses a shorter one because at 1200
// baud each flag is 4x quicker (80 flags = 533 ms vs a far more typical ~0.43 s
// here), trimming dead air and slot time without starving the receiver. Tune
// kVhf1200TxPreambleFlags if a transverter's T/R switching needs more lead-in.
constexpr int kTxPreambleFlags = 80;        // HF 300: ~2.13 s of flags
constexpr int kVhf1200TxPreambleFlags = 64; // VHF 1200: ~0.43 s of flags
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

// 1200 baud VHF (Bell 202 / APRS): one DPLL lane per slicer for each enabled
// algorithm. Duplicate suppression collapses the same frame seen by multiple
// lanes into one emission, like Direwolf's multi_modem.c.

// Profile A+ space-gain multipliers — exact Direwolf A+ values (MAX_SUBCHANS=9).
// Geometric series: MIN_G=0.5, MAX_G=4.0, 9 steps.
// Formula: gain[0]=MIN_G, gain[j]=gain[j-1]*pow(10, log10(MAX_G/MIN_G)/(N-1)).
constexpr std::array<float, 9> kVhf1200SpaceGains = {
    0.500000f, 0.648420f, 0.840896f, 1.090508f, 1.414214f,
    1.834008f, 2.378414f, 3.084422f, 4.000000f
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

QString framePreviewHex(const uint8_t* bytes, size_t byteCount)
{
    const qsizetype previewBytes = static_cast<qsizetype>(std::min<size_t>(byteCount, 24));
    QByteArray preview(reinterpret_cast<const char*>(bytes), previewBytes);
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

int txPreambleFlagsForProfile(Ax25ModemProfile profile)
{
    return profile == Ax25ModemProfile::Vhf1200 ? kVhf1200TxPreambleFlags : kTxPreambleFlags;
}

// Fill the rate / tone / preamble fields of a TX result from the active config
// and validate the sample-rate/baud combination. Returns false (with error set)
// if the combination cannot be rendered.
bool initTxResult(const Ax25DemodConfig& cfg, Ax25TransmitResult& result)
{
    result.sampleRate = cfg.sampleRate;
    result.baud = cfg.baud;
    result.polarity = cfg.polarity;
    result.markHz = cfg.markHz;
    result.spaceHz = cfg.spaceHz;
    result.preambleFlags = txPreambleFlagsForProfile(cfg.profile);
    result.postambleFlags = kTxPostambleFlags;
    result.vitaPacketFrames = kTxVitaPacketFrames;

    if (cfg.sampleRate <= 0 || cfg.baud <= 0 || cfg.sampleRate % cfg.baud != 0) {
        result.error = QStringLiteral("unsupported TX sample-rate/baud combination: %1 Hz / %2 baud")
            .arg(cfg.sampleRate)
            .arg(cfg.baud);
        return false;
    }
    return true;
}

// Modulate an encoded AX.25 bitstream into padded stereo float32 AFSK and fill
// the audio fields of the result. Shared by the text and KISS transmit paths.
void renderBitsToResult(const std::vector<uint8_t>& bits,
                        const Ax25DemodConfig& cfg,
                        Ax25TransmitResult& result)
{
    result.bitCount = static_cast<int>(bits.size());
    const int samplesPerSymbol = cfg.sampleRate / cfg.baud;
    const int payloadFrames = static_cast<int>(bits.size()) * samplesPerSymbol;
    const int paddedFrames = ((payloadFrames + kTxVitaPacketFrames - 1) / kTxVitaPacketFrames)
        * kTxVitaPacketFrames;
    result.audioFrames = paddedFrames;
    result.durationSeconds = static_cast<double>(paddedFrames) / static_cast<double>(cfg.sampleRate);
    result.stereoFloat32Pcm.resize(paddedFrames * 2 * static_cast<int>(sizeof(float)));

    const double mark = cfg.polarity == Ax25TonePolarity::Inverted ? cfg.spaceHz : cfg.markHz;
    const double space = cfg.polarity == Ax25TonePolarity::Inverted ? cfg.markHz : cfg.spaceHz;
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
}

void logTxSummary(const Ax25TransmitResult& result, const QString& via)
{
    qCInfo(lcAx25).noquote()
        << QStringLiteral("AX.25 TX packetized via=%1 SRC=%2 DST=%3 PATH=%4 payloadBytes=%5 "
                          "frameBytes=%6 bits=%7 samples=%8 duration=%9s levelRms=%10dBFS "
                          "levelPeak=%11dBFS baud=%12 mark=%13 space=%14 polarity=%15 "
                          "preamble=%16 postamble=%17")
            .arg(via,
                 result.frame.source,
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
                 : QStringLiteral("Reverse"))
            .arg(result.preambleFlags)
            .arg(result.postambleFlags);
}

// Best-effort decode of raw AX.25 frame bytes (no FCS) into display fields for
// the TX terminal/log. Returns a frame with payloadHex set if it cannot parse.
Ax25TransmitFrame transmitFrameFromBytes(const QByteArray& ax25NoFcs)
{
    Ax25TransmitFrame out;
    lm::address from;
    lm::address to;
    std::array<lm::address, 8> path = {};
    std::array<uint8_t, 256> data = {};
    uint8_t control = 0;
    uint8_t pid = 0;
    const auto* begin = reinterpret_cast<const uint8_t*>(ax25NoFcs.constData());
    const auto* end = begin + ax25NoFcs.size();

    auto [pathOut, dataOut, parsed] = lm::ax25::try_decode_frame_no_fcs(
        begin, end, from, to, path.begin(), data.begin(), data.size(), control, pid);
    if (!parsed) {
        out.payloadHex = QString::fromLatin1(ax25NoFcs.toHex(' ')).toUpper();
        return out;
    }

    out.source = addressToString(from);
    out.destination = addressToString(to);
    for (auto it = path.begin(); it != pathOut; ++it)
        out.path.append(addressToString(*it));
    out.control = control;
    out.pid = pid;
    const auto dataLength = static_cast<qsizetype>(std::distance(data.begin(), dataOut));
    out.payload = QByteArray(reinterpret_cast<const char*>(data.data()), dataLength);
    out.payloadText = Ax25FrameFormatter::payloadText(out.payload);
    out.payloadHex = Ax25FrameFormatter::payloadHex(out.payload);
    return out;
}

struct VhfLayout { bool wantA; int aSlicers; };

VhfLayout vhfModeLayout(VhfMode mode)
{
    bool wantA = false, aMulti = false;
    switch (mode) {
    case VhfMode::Off:                         break;
    case VhfMode::A:     wantA = true;         break;
    case VhfMode::APlus: wantA = true; aMulti = true; break;
    default: Q_UNREACHABLE();
    }
    return { wantA, aMulti ? static_cast<int>(kVhf1200SpaceGains.size()) : 1 };
}

} // namespace

Ax25DemodConfig ax25DemodConfigForProfile(Ax25ModemProfile profile, Ax25TonePolarity polarity, VhfMode vhfMode)
{
    Ax25DemodConfig config;
    config.profile = profile;
    config.sampleRate = 24000;
    config.polarity = polarity;
    config.vhfMode = vhfMode;

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
    struct DecodeLane {
        int phaseOffsetSamples{0};
        int samplesUntilStart{0};
        std::unique_ptr<IAfskDemod> demod;
        HdlcCodec hdlcCodec;
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
    quint64 lastHdlcFrameStartSampleIndex{0};
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

        auto addLaneA = [&](int phaseOffsetSamples, float spaceGain = 0.0f) {
            auto& lane = lanes.emplace_back();
            lane.phaseOffsetSamples = phaseOffsetSamples;
            lane.samplesUntilStart  = phaseOffsetSamples;
            // The pll_alpha positional arg (0.010) is accepted for API
            // compatibility but ignored by AetherAFSKDemod, which uses
            // Direwolf's internal DPLL inertia constants — so there is no
            // tunable VHF pll_alpha (unlike the HF libmodem lane below).
            lane.demod = std::make_unique<DirewolfAfskDemod>(
                mark, space, config.baud, config.sampleRate,
                0.75, 6.0, 0.75, 3.0, 0.008, 0.005, 0.010, spaceGain);
        };

        auto addLaneHf = [&](int phaseOffsetSamples, double pllAlpha) {
            auto& lane = lanes.emplace_back();
            lane.phaseOffsetSamples = phaseOffsetSamples;
            lane.samplesUntilStart  = phaseOffsetSamples;
            lane.demod = std::make_unique<LibmodemAfskDemod>(
                mark, space, config.baud, config.sampleRate,
                0.75, 6.0, 0.75, 3.0, 0.008, 0.005, pllAlpha);
        };

        if (config.profile == Ax25ModemProfile::Hf300) {
            // HF: free-running lanes (pll_alpha 0), recovery by phase diversity.
            for (int phaseOffset : kHf300DecodePhaseOffsets)
                addLaneHf(phaseOffset, 0.0);
        } else {
            // VHF 1200: one DPLL lane per slicer for each enabled algorithm.
            const auto layout = vhfModeLayout(config.vhfMode);
            if (layout.wantA)
                for (int s = 0; s < layout.aSlicers; ++s)
                    addLaneA(0, layout.aSlicers > 1 ? kVhf1200SpaceGains[s] : 0.0f);
        }
        resetDecoderState(true, true);

        qCInfo(lcAx25).noquote()
            << QStringLiteral("modem configured: %1 sampleRate=%2Hz baud=%3 samplesPerSymbol=%4 "
                              "mark=%5Hz space=%6Hz polarity=%7 lanes=%8")
                   .arg(ax25ModemProfileName(config.profile))
                   .arg(config.sampleRate)
                   .arg(config.baud)
                   .arg(config.baud > 0 ? config.sampleRate / config.baud : 0)
                   .arg(mark, 0, 'f', 0)
                   .arg(space, 0, 'f', 0)
                   .arg(config.polarity == Ax25TonePolarity::Inverted
                        ? QStringLiteral("Reverse")
                        : QStringLiteral("Normal"))
                   .arg(lanes.size());
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
            lastHdlcFrameStartSampleIndex = 0;
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
        lane.hdlcCodec.reset();
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
                if (diagnosticsLoggingEnabled) {
                    qCDebug(lcAx25).nospace()
                        << "receive gate opened: rms="
                        << QString::number(receiveGateRmsDbfs, 'f', 1) << "dBFS floor="
                        << QString::number(receiveGateFloorDbfs, 'f', 1) << "dBFS opens="
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

    bool candidateHasAx25Structure(const uint8_t* frameBytes,
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
            frameBytes,
            frameBytes + frameBytesSize - 2,
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
        lastRejectFrameBits = lane.hdlcCodec.frameSizeBits();
        lastRejectFrameBytes = static_cast<int>(frameBytesSize);
        lastRejectPreviewHex = framePreviewHex(lane.hdlcCodec.frameData(), frameBytesSize);
        lastRejectActualFcs = frameBytesSize >= 17 ? fcsToString(actualFcs) : QString();
        lastRejectExpectedFcs = frameBytesSize >= 17 ? fcsToString(expectedFcs) : QString();

        // Minimum valid AX.25 frame is 17 bytes (14 address + 1 control + 2 FCS);
        // a no-PID U-frame (SABM/DISC/UA/DM) sits exactly at 17. Anything shorter
        // is a noise-triggered flag pair.
        if (frameBytesSize < 17) {
            ++totalRejectTooShort;
            lastRejectReason = QStringLiteral("too-short");
            return false;
        }

        uint8_t control = 0;
        uint8_t pid = 0;
        size_t pathCount = 0;
        size_t dataLength = 0;
        const bool ax25Like = candidateHasAx25Structure(lane.hdlcCodec.frameData(), frameBytesSize, control, pid, pathCount, dataLength);

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
        const bool wasInFrame = lane.hdlcCodec.inFrame();

        const bool frameComplete = lane.hdlcCodec.processBit(bit ? 1 : 0);

        if (lane.hdlcCodec.inFrame() && !wasInFrame) {
            // One symbol = 20 samples at 24 kHz/1200 baud; window suppresses
            // cross-lane duplicates.  Counter is approximate: DPLLs on
            // different lanes may land frame-start detection slightly apart.
            if (currentDecodeSampleIndex > lastHdlcFrameStartSampleIndex + 20) {
                lastHdlcFrameStartSampleIndex = currentDecodeSampleIndex;
                ++totalHdlcFrameStarts;
            }
        }

        if (!frameComplete)
            return std::nullopt;

        ++totalHdlcFrameCandidates;

        const size_t frameSize    = lane.hdlcCodec.frameSize();
        const uint8_t* frameBytes = lane.hdlcCodec.frameData();
        const auto actualFcs      = lane.hdlcCodec.actualFcs();
        const auto expectedFcs    = lane.hdlcCodec.expectedFcs();

        if (!lane.hdlcCodec.fcsValid()) {
            if (recordReject(lane, frameSize, actualFcs, expectedFcs))
                ++totalPlausibleAx25Candidates;
            return std::nullopt;
        }

        lm::address from;
        lm::address to;
        std::array<lm::address, 8> path = {};
        std::array<uint8_t, 256> data = {};
        uint8_t control = 0;
        uint8_t pid = 0;

        auto [pathOut, dataOut, parsed] = lm::ax25::try_decode_frame_no_fcs(
            frameBytes,
            frameBytes + frameSize - 2,
            from,
            to,
            path.begin(),
            data.begin(),
            data.size(),
            control,
            pid);

        const std::array<uint8_t, 2> acceptedFcs = { 0, 0 };
        const bool ax25Valid = parsed && lm::ax25::validate_frame(
            from, to,
            path.begin(), pathOut,
            data.begin(), dataOut,
            control, pid,
            acceptedFcs, acceptedFcs);

        if (!ax25Valid) {
            if (recordReject(lane, frameSize, actualFcs, expectedFcs))
                ++totalPlausibleAx25Candidates;
            return std::nullopt;
        }

        ++totalPlausibleAx25Candidates;

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
        Ax25DecodedFrame decodedFrame = toDecodedFrame(frame, lane.lastQuality, lane.phaseOffsetSamples);
        // Capture on-air frame bytes minus 2-byte FCS for KISS TNC forwarding.
        if (frameSize >= 2) {
            decodedFrame.ax25FrameNoFcs = QByteArray(
                reinterpret_cast<const char*>(frameBytes),
                static_cast<qsizetype>(frameSize - 2));
        }
        return decodedFrame;
    }

    bool shouldEmitFrame(const Ax25DecodedFrame& frame)
    {
        const QByteArray signature = frameSignature(frame);
        const quint64 duplicateWindowSamples =
            static_cast<quint64>(std::max(1, config.sampleRate)) * kDuplicateSuppressSeconds;

        auto sampleGap = [&](quint64 a) -> quint64 {
            return currentDecodeSampleIndex >= a
                ? currentDecodeSampleIndex - a
                : a - currentDecodeSampleIndex;
        };

        recentFrames.erase(std::remove_if(recentFrames.begin(),
                                          recentFrames.end(),
                                          [&](const RecentFrame& recent) {
                                              return sampleGap(recent.sampleIndex) > duplicateWindowSamples;
                                          }),
                           recentFrames.end());

        const auto duplicate = std::find_if(recentFrames.begin(),
                                            recentFrames.end(),
                                            [&](const RecentFrame& recent) {
                                                return recent.signature == signature
                                                    && sampleGap(recent.sampleIndex) <= duplicateWindowSamples;
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

    void recordDemodSymbol(uint8_t bit, double confidence)
    {
        ++diagnosticsWindow.demodSymbols;
        diagnosticsWindow.oneBits += bit ? 1 : 0;
        diagnosticsWindow.confidenceSum += confidence;
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
            diagnostics.searching = diagnostics.searching && lane.hdlcCodec.searching();
            diagnostics.inPreamble = diagnostics.inPreamble || lane.hdlcCodec.inPreamble();
            diagnostics.inFrame = diagnostics.inFrame || lane.hdlcCodec.inFrame();
            diagnostics.aborted = diagnostics.aborted || lane.hdlcCodec.aborted();
            diagnostics.currentFrameBits = std::max(diagnostics.currentFrameBits,
                                                    lane.hdlcCodec.bitstreamSize());
            diagnostics.lastFrameBits = std::max(diagnostics.lastFrameBits,
                                                 lane.hdlcCodec.frameSizeBits());
            diagnostics.preambleFlags = std::max(diagnostics.preambleFlags,
                                                 lane.hdlcCodec.preambleCount());
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
    // lanes.empty() when VhfMode::Off — diagnostics stay frozen intentionally.
    if (!samples || sampleCount <= 0 || m_impl->lanes.empty())
        return frames;

    if (sampleRate != m_impl->config.sampleRate) {
        // TODO: Wire an existing project resampler here if a future tap emits
        // anything other than the native 24 kHz remote_audio_rx stream.
        return frames;
    }

    m_impl->updateReceiveGate(samples, sampleCount, sampleRate);

    // Normalise and record audio metrics in one pass.
    QVarLengthArray<float, 2048> norm(sampleCount);
    for (int i = 0; i < sampleCount; ++i) {
        norm[i] = std::isfinite(samples[i]) ? std::clamp(samples[i], -1.0f, 1.0f) : 0.0f;
        m_impl->recordAudioSample(norm[i], sampleRate);
    }

    const quint64 baseIndex = m_impl->totalAudioSamplesProcessed;
    m_impl->totalAudioSamplesProcessed += static_cast<quint64>(sampleCount);

    std::vector<BitResult> laneBits;
    laneBits.reserve(static_cast<size_t>(sampleCount / 20 + 4));

    for (size_t laneIndex = 0; laneIndex < m_impl->lanes.size(); ++laneIndex) {
        auto& lane = m_impl->lanes[laneIndex];
        if (!lane.demod) continue;

        const int skip = std::min(lane.samplesUntilStart, sampleCount);
        lane.samplesUntilStart -= skip;
        if (skip >= sampleCount) continue;

        laneBits.clear();
        lane.demod->processBlock(norm.constData() + skip, sampleCount - skip,
                                  baseIndex + static_cast<quint64>(skip), laneBits);

        for (const auto& b : laneBits) {
            m_impl->currentDecodeSampleIndex = b.sampleIndex;
            if (laneIndex == m_impl->lanes.size() / 2)
                m_impl->recordDemodSymbol(b.bit, b.confidence);
            if (auto decoded = m_impl->processBit(lane, b.bit, b.confidence);
                decoded && m_impl->shouldEmitFrame(*decoded)) {
                frames.append(*decoded);
            }
        }
    }
    return frames;
}

QVector<Ax25DecodedFrame> AetherAx25LibmodemShim::processRecoveredBitsForTest(
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

int ax25DemodLaneCount(const Ax25DemodConfig& cfg)
{
    if (cfg.profile == Ax25ModemProfile::Hf300)
        return static_cast<int>(kHf300DecodePhaseOffsets.size());

    const auto layout = vhfModeLayout(cfg.vhfMode);
    return layout.wantA ? layout.aSlicers : 0;
}

QString ax25DemodDescription(const Ax25DemodConfig& cfg)
{
    const int lanes = ax25DemodLaneCount(cfg);
    return QStringLiteral("%1: %2 Hz, %3 bps, mark %4 Hz, space %5 Hz, %6, %7 lane%8")
        .arg(ax25ModemProfileName(cfg.profile))
        .arg(cfg.sampleRate)
        .arg(cfg.baud)
        .arg(cfg.markHz, 0, 'f', 0)
        .arg(cfg.spaceHz, 0, 'f', 0)
        .arg(cfg.polarity == Ax25TonePolarity::Normal
             ? QStringLiteral("Normal")
             : QStringLiteral("Inverted"))
        .arg(lanes)
        .arg(lanes == 1 ? QString() : QStringLiteral("s"));
}

Ax25TransmitResult ax25BuildTransmitAudio(
    const Ax25DemodConfig& cfg,
    const QString& text,
    const QString& defaultSource,
    const QString& defaultDestination)
{
    Ax25TransmitResult result;
    if (!initTxResult(cfg, result))
        return result;

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
        frameBytes, 0, result.preambleFlags, kTxPostambleFlags);
    result.frameBytes = static_cast<int>(frameBytes.size());
    renderBitsToResult(bits, cfg, result);
    result.ok = true;
    logTxSummary(result, QStringLiteral("text"));
    return result;
}

Ax25TransmitResult AetherAx25LibmodemShim::buildTransmitAudio(
    const QString& text,
    const QString& defaultSource,
    const QString& defaultDestination) const
{
    return ax25BuildTransmitAudio(m_impl->config, text, defaultSource, defaultDestination);
}

Ax25TransmitResult ax25BuildTransmitAudioFromFrame(
    const Ax25DemodConfig& cfg,
    const QByteArray& ax25NoFcs)
{
    Ax25TransmitResult result;
    if (!initTxResult(cfg, result))
        return result;

    // Minimum valid AX.25 frame without FCS: 14 address bytes + 1 control byte = 15.
    if (ax25NoFcs.size() < 15) {
        result.error = QStringLiteral("KISS frame too short: %1 bytes (need >= 15)")
            .arg(ax25NoFcs.size());
        return result;
    }

    // KISS host omits FCS; the TNC appends it before encoding.
    std::vector<uint8_t> frameBytes(
        reinterpret_cast<const uint8_t*>(ax25NoFcs.constData()),
        reinterpret_cast<const uint8_t*>(ax25NoFcs.constData()) + ax25NoFcs.size());
    const std::array<uint8_t, 2> crc = lm::ax25::compute_crc(frameBytes.begin(), frameBytes.end());
    frameBytes.push_back(crc[0]);
    frameBytes.push_back(crc[1]);

    const std::vector<uint8_t> bits = lm::ax25::encode_bitstream(
        frameBytes, 0, result.preambleFlags, kTxPostambleFlags);
    result.frameBytes = static_cast<int>(frameBytes.size());
    result.frame = transmitFrameFromBytes(ax25NoFcs);
    renderBitsToResult(bits, cfg, result);
    result.ok = true;
    logTxSummary(result, QStringLiteral("kiss"));
    return result;
}

Ax25TransmitResult AetherAx25LibmodemShim::buildTransmitAudioFromFrame(
    const QByteArray& ax25NoFcs) const
{
    return ax25BuildTransmitAudioFromFrame(m_impl->config, ax25NoFcs);
}

Ax25DecoderDiagnostics AetherAx25LibmodemShim::diagnosticsSnapshot() const
{
    return m_impl->makeDiagnostics(m_impl->config.sampleRate);
}

QString AetherAx25LibmodemShim::demodDescription() const
{
    return ax25DemodDescription(m_impl->config);
}

void AetherAx25LibmodemShim::feedAudio(const QByteArray& monoFloat32Pcm, int sampleRate)
{
    const int sampleCount = monoFloat32Pcm.size() / static_cast<int>(sizeof(float));
    const auto* samples = reinterpret_cast<const float*>(monoFloat32Pcm.constData());
    const QVector<Ax25DecodedFrame> frames = processMonoFloat(samples, sampleCount, sampleRate);
    for (const auto& frame : frames) {
        qCInfo(lcAx25).noquote()
            << QStringLiteral("decoded AX.25 frame baud=%1 conf=%2 SRC=%3 DST=%4 VIA=%5 UI=%6 pid=%7 phase=%8 payload=%9")
                .arg(m_impl->config.baud)
                .arg(frame.confidenceOrQuality, 0, 'f', 2)
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
                << "baud=" << m_impl->config.baud
                << " sr=" << diagnostics->sampleRate
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

} // namespace AetherSDR
