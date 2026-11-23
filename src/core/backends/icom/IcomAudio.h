#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "core/backends/icom/IcomProtocol.h"

// Audio payload handling for the Icom RS-BA1 audio stream.
//
// Deliberately narrow. The transport (sequencing, retransmission, keepalives)
// is IcomStream's job; this file only turns payload bytes into samples and back,
// and packetises transmit audio into the shape the radio expects.
//
// Qt-free; icom_audio_test drives it directly.
//
// See ~/oracles/icom/icom-oracle.md §7.

namespace AetherSDR::icom {

// ---------------------------------------------------------------------------
// Codecs
// ---------------------------------------------------------------------------

// We negotiate LPCM 1ch 16-bit (codec 4), which needs no decoder. The 8-bit and
// μ-law forms exist for constrained WAN links; Opus (64) and ADPCM (128) are not
// implemented. An unimplemented codec must be REFUSED at negotiation — requesting
// it and mis-decoding puts full-scale noise in the operator's headphones.
[[nodiscard]] bool codecSupported(AudioCodec c) noexcept;
[[nodiscard]] int  codecChannels(AudioCodec c) noexcept;

// Decode a payload to interleaved float samples in [-1, 1].
[[nodiscard]] std::vector<float> decodeAudio(AudioCodec codec,
                                              std::span<const std::uint8_t> payload);

// Encode mono float samples to the wire format.
[[nodiscard]] std::vector<std::uint8_t> encodeAudio(AudioCodec codec,
                                                     std::span<const float> mono);

// ---------------------------------------------------------------------------
// Transmit packetisation
// ---------------------------------------------------------------------------

// A 20 ms frame at 48 kHz mono s16 is 1920 bytes, which the protocol splits
// across two packets of 1364 + 556 bytes (both directions). This class owns that
// split plus the buffering from AudioEngine's arbitrary block sizes.
// Underflow sends SILENCE (repeating a frame puts an artefact on air; blocking
// starves the stream keepalives). Overflow drops the OLDEST audio.
class TxPacketizer {
public:
    struct Chunk {
        std::vector<std::uint8_t> bytes;
    };

    explicit TxPacketizer(AudioCodec codec = AudioCodec::Lpcm1ch16) : m_codec(codec) {}

    // Queue mono float samples. Extra samples beyond the cap displace the
    // oldest.
    void submit(std::span<const float> mono);

    // Pull the next pair of chunks if a full 20 ms frame is available. Returns
    // an empty vector when there is not yet enough audio — the caller sends
    // nothing rather than sending a short frame, because the radio's jitter
    // buffer treats a short packet as a discontinuity.
    [[nodiscard]] std::vector<Chunk> takeFrame();

    // Discard everything pending. Call on UNKEY: whatever is still queued
    // belongs to the transmission that just ended, and sending it after the
    // carrier drops is at best confusing and at worst a stray emission.
    void flush() noexcept;

    // Complete the final 20 ms wire frame with codec-correct silence. Returns
    // the number of bytes appended, or zero when already frame-aligned. May
    // exceed kMaxPendingBytes by less than one frame; it never evicts.
    [[nodiscard]] std::size_t padToFrame();

    [[nodiscard]] std::size_t pendingBytes() const noexcept { return m_pending.size(); }

    // Roughly 250 ms at 48 kHz mono s16. Past that the operator is hearing
    // their own latency, so dropping beats growing the backlog.
    static constexpr std::size_t kMaxPendingBytes = 24000;

private:
    AudioCodec m_codec;
    std::deque<std::uint8_t> m_pending;
};

// ---------------------------------------------------------------------------
// Receive reassembly
// ---------------------------------------------------------------------------

// Decode ordered payloads without changing their sample clock. Mono uses an
// unequal pair per 20 ms; native stereo uses three fragments per 20 ms. UDP
// idles consume outer sequence numbers but contain no audio samples.
class RxAssembler {
public:
    explicit RxAssembler(AudioCodec codec = AudioCodec::Lpcm1ch16) : m_codec(codec) {}

    // Decode one in-order payload. Returns the samples it contained.
    [[nodiscard]] std::vector<float> accept(std::span<const std::uint8_t> payload);

    // Legacy mono loss estimate. Stereo must use the media sequence below:
    // outer packet loss includes idles and does not establish missing samples.
    [[nodiscard]] std::vector<float> concealLoss(int packets, int sampleRateHz);

    // Learn two consecutive native stereo cycles before filling missing media
    // fragments. Each returned block occupies exactly the missing fragment's
    // sample frames on BOTH channels; silence is concealment, not recovery.
    [[nodiscard]] std::vector<std::vector<float>> concealBefore(
        std::span<const std::uint8_t> payload, std::uint16_t mediaSequence);

private:
    AudioCodec m_codec;
    std::vector<std::uint8_t> m_partialFrame; // At most three stereo PCM bytes.
    std::optional<std::uint16_t> m_mediaSequence;
    std::array<std::size_t,3> m_fragmentBytes{};
    int m_fragmentPhase = 0;
    int m_consecutiveFragments = 0;
};

}  // namespace AetherSDR::icom
