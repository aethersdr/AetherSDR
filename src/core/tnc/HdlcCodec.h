#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

namespace AetherSDR {

// Pure C++ HDLC framing for AX.25.  Processes one NRZI tone at a time.
// Replaces lm::ax25::bitstream_state + lm::ax25::try_decode_bitstream in the
// VHF 1200-baud decode path, keeping libmodem only for HF demod and AX.25
// structure parsing.  No Qt, no libmodem, no Direwolf derivation.
//
// Algorithm: Direwolf-style shift-register flag/stuff/abort detection with
// incremental byte assembly and batch CRC validation at frame close.
class HdlcCodec {
public:
    // Maximum frame size in bytes (includes 2 FCS bytes).
    // AX.25 max frame body is ~330 bytes; 512 gives comfortable headroom.
    static constexpr size_t kMaxFrameBytes = 512;

    void reset();

    // Feed one NRZI tone (1 = mark, 0 = space).
    // Returns true the call that closes a frame (complete() transitions false→true).
    // complete() stays true until the next processBit() call.
    bool processBit(uint8_t nrziTone);

    // State accessors (valid immediately after processBit returns).
    bool searching()  const { return m_state == State::Searching; }
    bool inPreamble() const { return m_state == State::InPreamble; }
    bool inFrame()    const { return m_state == State::InFrame; }
    bool complete()   const { return m_complete; }
    bool aborted()    const { return m_aborted; }
    bool fcsValid()   const { return m_fcsValid; }

    // Raw-bit count for the current in-progress frame (diagnostic).
    int bitstreamSize() const;

    // Bit length of the last completed frame (set at frame close).
    int frameSizeBits()  const { return m_frameSizeBits; }
    int preambleCount()  const { return m_preambleCount; }

    // Frame bytes including the 2-byte FCS field.  Valid from the
    // processBit() call that set complete() until reset() is called.
    const uint8_t* frameData() const { return m_frameBuffer.data(); }
    size_t         frameSize() const { return m_frameByteCount; }

    // FCS bytes: actual = last 2 bytes of frame; expected = CRC of frame[0..N-3].
    std::array<uint8_t, 2> actualFcs()   const { return m_actualFcs; }
    std::array<uint8_t, 2> expectedFcs() const { return m_expectedFcs; }

private:
    enum class State : uint8_t { Searching, InPreamble, InFrame };

    bool closeFrame();
    void accumulateBit(uint8_t bit);
    void beginFrame();

    State   m_state{State::Searching};
    bool    m_complete{false};
    bool    m_aborted{false};
    bool    m_fcsValid{false};

    uint8_t m_prevNrzi{0};
    uint8_t m_patDet{0};   // rolling 8-bit pattern, newest bit at MSB

    // Incremental byte assembly
    uint8_t m_currentByte{0};
    int     m_bitIndex{0};

    // Frame storage
    std::array<uint8_t, kMaxFrameBytes> m_frameBuffer{};
    size_t m_frameByteCount{0};

    // Diagnostics
    int m_frameSizeBits{0};
    int m_preambleCount{0};
    int m_rawBitsInFrame{0};   // raw bits seen while in InFrame (not unstuffed)
    int m_bitsAfterFlag{0};    // non-flag bits since last preamble flag; require >=8 to enter InFrame

    std::array<uint8_t, 2> m_actualFcs{};
    std::array<uint8_t, 2> m_expectedFcs{};
};

} // namespace AetherSDR
