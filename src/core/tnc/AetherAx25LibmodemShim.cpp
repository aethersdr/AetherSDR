#include "core/tnc/AetherAx25LibmodemShim.h"

#include "core/tnc/Ax25FrameFormatter.h"

#include "bitstream.h"
#include "demodulator.h"

#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace AetherSDR {

namespace lm = aether_libmodem_core;

namespace {

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
    double lastQuality{0.0};

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
            0.015);
        resetBitstream();
    }

    void resetBitstream()
    {
        bitstreamState.reset();
        bitstreamState.max_frame_bits = 4096;
        bitstreamBuffer.clear();
        bitstreamBuffer.resize(8192);
        lastQuality = 0.0;
    }

    std::optional<Ax25DecodedFrame> processBit(uint8_t bit, double quality)
    {
        lm::ax25::frame frame;
        lastQuality = 0.95 * lastQuality + 0.05 * quality;
        if (!lm::ax25::try_decode_bitstream(bit ? 1 : 0, bitstreamState, bitstreamBuffer, frame))
            return std::nullopt;
        return toDecodedFrame(frame, lastQuality);
    }
};

AetherAx25LibmodemShim::AetherAx25LibmodemShim(QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
    qRegisterMetaType<AetherSDR::Ax25DecodedFrame>("AetherSDR::Ax25DecodedFrame");
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
    if (m_impl->demod)
        m_impl->demod->reset();
    m_impl->resetBitstream();
    emit statusChanged();
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

    for (int i = 0; i < sampleCount; ++i) {
        const float sample = std::isfinite(samples[i])
            ? std::clamp(samples[i], -1.0f, 1.0f)
            : 0.0f;
        lm::demod_result result;
        if (!m_impl->demod->try_demodulate(sample, result))
            continue;
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
    for (const auto& frame : frames)
        emit frameDecoded(frame);
    if (!frames.isEmpty())
        emit statusChanged();
}

} // namespace AetherSDR
