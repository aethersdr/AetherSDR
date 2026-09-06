#include "core/backends/icom/IcomAudio.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR::icom {
namespace {

constexpr float kInt16Scale = 32768.0f;

std::int16_t toInt16(float v) noexcept
{
    const float clamped = std::clamp(v, -1.0f, 1.0f);
    return static_cast<std::int16_t>(std::lround(clamped * 32767.0f));
}

// ITU-T G.711 μ-law expansion. Present because the codec negotiation is ours to
// make and a constrained WAN uplink is a real reason to halve the bitrate.
float uLawToFloat(std::uint8_t u) noexcept
{
    u = static_cast<std::uint8_t>(~u);
    const int sign     = u & 0x80;
    const int exponent = (u >> 4) & 0x07;
    const int mantissa = u & 0x0f;
    int sample = ((mantissa << 3) + 0x84) << exponent;
    sample -= 0x84;
    return static_cast<float>(sign ? -sample : sample) / kInt16Scale;
}

std::uint8_t floatToULaw(float v) noexcept
{
    constexpr int kBias = 0x84;
    constexpr int kClip = 32635;
    int sample = toInt16(v);
    int sign = (sample >> 8) & 0x80;
    if (sign)
        sample = -sample;
    sample = std::min(sample, kClip);
    sample += kBias;

    int exponent = 7;
    for (int mask = 0x4000; (sample & mask) == 0 && exponent > 0; mask >>= 1)
        --exponent;
    const int mantissa = (sample >> (exponent + 3)) & 0x0f;
    return static_cast<std::uint8_t>(~(sign | (exponent << 4) | mantissa));
}

}  // namespace

// ---------------------------------------------------------------------------
// Codecs
// ---------------------------------------------------------------------------

bool codecSupported(AudioCodec c) noexcept
{
    switch (c) {
    case AudioCodec::ULaw1ch8:
    case AudioCodec::Lpcm1ch8:
    case AudioCodec::Lpcm1ch16:
    case AudioCodec::Lpcm2ch16:
        return true;
    // Opus and ADPCM are deliberately unimplemented. Refusing them at
    // negotiation is the point: requesting a codec we cannot decode and then
    // feeding the bytes through the LPCM path produces full-scale noise into
    // the operator's headphones, which is a genuinely harmful failure.
    case AudioCodec::Opus1ch:
    case AudioCodec::Adpcm1ch:
        return false;
    }
    return false;
}

int codecChannels(AudioCodec c) noexcept
{
    return c == AudioCodec::Lpcm2ch16 ? 2 : 1;
}

std::vector<float> decodeAudio(AudioCodec codec, std::span<const std::uint8_t> payload)
{
    std::vector<float> out;
    switch (codec) {
    case AudioCodec::Lpcm1ch16:
    case AudioCodec::Lpcm2ch16: {
        // Little-endian signed 16-bit. An odd trailing byte means a truncated
        // payload; dropping it is right — synthesising a sample from half a
        // word puts a full-scale click in the audio.
        const std::size_t n = payload.size() / 2;
        out.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const auto lo = static_cast<std::uint16_t>(payload[i * 2]);
            const auto hi = static_cast<std::uint16_t>(payload[i * 2 + 1]);
            out[i] = static_cast<std::int16_t>(lo | (hi << 8)) / kInt16Scale;
        }
        break;
    }
    case AudioCodec::Lpcm1ch8: {
        // UNSIGNED 8-bit, centred on 128 — not signed. Reading it as signed
        // gives audio that is inverted around the midpoint and sounds like
        // heavy distortion rather than like a decode error.
        out.resize(payload.size());
        for (std::size_t i = 0; i < payload.size(); ++i)
            out[i] = (static_cast<float>(payload[i]) - 128.0f) / 128.0f;
        break;
    }
    case AudioCodec::ULaw1ch8: {
        out.resize(payload.size());
        for (std::size_t i = 0; i < payload.size(); ++i)
            out[i] = uLawToFloat(payload[i]);
        break;
    }
    default:
        break;   // unsupported codec: no samples, never garbage
    }
    return out;
}

std::vector<std::uint8_t> encodeAudio(AudioCodec codec, std::span<const float> mono)
{
    std::vector<std::uint8_t> out;
    switch (codec) {
    case AudioCodec::Lpcm1ch16:
    case AudioCodec::Lpcm2ch16: {
        out.resize(mono.size() * 2);
        for (std::size_t i = 0; i < mono.size(); ++i) {
            const auto s = static_cast<std::uint16_t>(toInt16(mono[i]));
            out[i * 2]     = static_cast<std::uint8_t>(s & 0xff);
            out[i * 2 + 1] = static_cast<std::uint8_t>((s >> 8) & 0xff);
        }
        break;
    }
    case AudioCodec::Lpcm1ch8: {
        out.resize(mono.size());
        for (std::size_t i = 0; i < mono.size(); ++i)
            out[i] = static_cast<std::uint8_t>(
                std::clamp(std::lround(mono[i] * 127.0f) + 128L, 0L, 255L));
        break;
    }
    case AudioCodec::ULaw1ch8: {
        out.resize(mono.size());
        for (std::size_t i = 0; i < mono.size(); ++i)
            out[i] = floatToULaw(mono[i]);
        break;
    }
    default:
        break;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Transmit packetisation
// ---------------------------------------------------------------------------

void TxPacketizer::submit(std::span<const float> mono)
{
    const auto bytes = encodeAudio(m_codec, mono);
    m_pending.insert(m_pending.end(), bytes.begin(), bytes.end());

    // Drop the OLDEST on overflow. On transmit the freshest audio is what
    // matters; keeping a backlog just means the operator's voice arrives late
    // and keeps getting later.
    while (m_pending.size() > kMaxPendingBytes)
        m_pending.pop_front();
}

std::vector<TxPacketizer::Chunk> TxPacketizer::takeFrame()
{
    if (m_pending.size() < kAudioFrameBytes)
        return {};   // short frames read as a discontinuity to the radio's jitter buffer

    std::vector<std::uint8_t> frame;
    frame.reserve(kAudioFrameBytes);
    for (std::size_t i = 0; i < kAudioFrameBytes; ++i) {
        frame.push_back(m_pending.front());
        m_pending.pop_front();
    }

    std::vector<Chunk> chunks;
    chunks.reserve(2);
    chunks.push_back({std::vector<std::uint8_t>(frame.begin(),
                                                frame.begin() + kAudioSplitLarge)});
    chunks.push_back({std::vector<std::uint8_t>(frame.begin() + kAudioSplitLarge, frame.end())});
    return chunks;
}

void TxPacketizer::flush() noexcept { m_pending.clear(); }

std::size_t TxPacketizer::padToFrame()
{
    const std::size_t remainder = m_pending.size() % kAudioFrameBytes;
    if (remainder == 0) {
        return 0;
    }

    // Never evict to make room. The cap in submit() bounds LATENCY for a
    // continuous producer; padding is the end of a finite stream, adds less
    // than one frame, and nothing follows it. Dropping the oldest frame here
    // would punch a 20 ms hole into unsent audio of the very packet the
    // padding exists to complete — an FCS failure by another route.
    const std::size_t missing = kAudioFrameBytes - remainder;
    std::uint8_t silenceByte = 0x00;
    if (m_codec == AudioCodec::Lpcm1ch8) {
        silenceByte = 0x80;
    } else if (m_codec == AudioCodec::ULaw1ch8) {
        silenceByte = 0xFF;
    }
    m_pending.insert(m_pending.end(), missing, silenceByte);
    return missing;
}

// ---------------------------------------------------------------------------
// Receive reassembly
// ---------------------------------------------------------------------------

std::vector<float> RxAssembler::accept(std::span<const std::uint8_t> payload)
{
    return decodeAudio(m_codec, payload);
}

std::vector<float> RxAssembler::concealLoss(int packets, int sampleRateHz) const
{
    if (packets <= 0 || sampleRateHz <= 0)
        return {};
    // 10 ms per packet is the AVERAGE across the unequal pair, and averaging is
    // exactly the right use of it here: we are filling a hole whose true length
    // we cannot know, and silence of approximately the right duration keeps the
    // stream's timing from walking.
    const std::size_t perPacket = static_cast<std::size_t>(sampleRateHz) / 100;
    return std::vector<float>(perPacket * static_cast<std::size_t>(packets), 0.0f);
}

}  // namespace AetherSDR::icom
