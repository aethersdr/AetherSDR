#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
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

// The receive counterpart. The radio sends the same unequal pair, and the
// naive reading — "each packet is 10 ms" — is an AVERAGE across the two, not a
// per-packet truth: they carry 14.2 ms and 5.8 ms.
//
// So this class deliberately does NOT timestamp packets. It concatenates
// payloads in the order the sequencer delivered them and lets the audio device
// clock the result. A backend that derives timing from packet size drifts.
class RxAssembler {
public:
    explicit RxAssembler(AudioCodec codec = AudioCodec::Lpcm1ch16) : m_codec(codec) {}

    // Decode one in-order payload. Returns the samples it contained.
    [[nodiscard]] std::vector<float> accept(std::span<const std::uint8_t> payload);

    // Samples the caller should insert for `count` lost packets, so a gap the
    // retransmitter could not repair becomes a brief silence rather than a
    // click. Sized from the average packet, which is the right use of the 10 ms
    // figure — an approximation standing in for data we do not have.
    [[nodiscard]] std::vector<float> concealLoss(int packets, int sampleRateHz) const;

private:
    AudioCodec m_codec;
};

}  // namespace AetherSDR::icom
