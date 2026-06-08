#include "HdlcCodec.h"

namespace AetherSDR {

// CRC-16-CCITT lookup table, polynomial 0x8408 (bit-reversed 0x1021).
// init=0xFFFF, final XOR=0xFFFF — identical to Direwolf fcs_calc.c and
// libmodem compute_crc_using_lut.  Verified against both implementations.
static constexpr uint16_t kCrcTable[256] = {
    0x0000, 0x1189, 0x2312, 0x329B, 0x4624, 0x57AD, 0x6536, 0x74BF,
    0x8C48, 0x9DC1, 0xAF5A, 0xBED3, 0xCA6C, 0xDBE5, 0xE97E, 0xF8F7,
    0x1081, 0x0108, 0x3393, 0x221A, 0x56A5, 0x472C, 0x75B7, 0x643E,
    0x9CC9, 0x8D40, 0xBFDB, 0xAE52, 0xDAED, 0xCB64, 0xF9FF, 0xE876,
    0x2102, 0x308B, 0x0210, 0x1399, 0x6726, 0x76AF, 0x4434, 0x55BD,
    0xAD4A, 0xBCC3, 0x8E58, 0x9FD1, 0xEB6E, 0xFAE7, 0xC87C, 0xD9F5,
    0x3183, 0x200A, 0x1291, 0x0318, 0x77A7, 0x662E, 0x54B5, 0x453C,
    0xBDCB, 0xAC42, 0x9ED9, 0x8F50, 0xFBEF, 0xEA66, 0xD8FD, 0xC974,
    0x4204, 0x538D, 0x6116, 0x709F, 0x0420, 0x15A9, 0x2732, 0x36BB,
    0xCE4C, 0xDFC5, 0xED5E, 0xFCD7, 0x8868, 0x99E1, 0xAB7A, 0xBAF3,
    0x5285, 0x430C, 0x7197, 0x601E, 0x14A1, 0x0528, 0x37B3, 0x263A,
    0xDECD, 0xCF44, 0xFDDF, 0xEC56, 0x98E9, 0x8960, 0xBBFB, 0xAA72,
    0x6306, 0x728F, 0x4014, 0x519D, 0x2522, 0x34AB, 0x0630, 0x17B9,
    0xEF4E, 0xFEC7, 0xCC5C, 0xDDD5, 0xA96A, 0xB8E3, 0x8A78, 0x9BF1,
    0x7387, 0x620E, 0x5095, 0x411C, 0x35A3, 0x242A, 0x16B1, 0x0738,
    0xFFCF, 0xEE46, 0xDCDD, 0xCD54, 0xB9EB, 0xA862, 0x9AF9, 0x8B70,
    0x8408, 0x9581, 0xA71A, 0xB693, 0xC22C, 0xD3A5, 0xE13E, 0xF0B7,
    0x0840, 0x19C9, 0x2B52, 0x3ADB, 0x4E64, 0x5FED, 0x6D76, 0x7CFF,
    0x9489, 0x8500, 0xB79B, 0xA612, 0xD2AD, 0xC324, 0xF1BF, 0xE036,
    0x18C1, 0x0948, 0x3BD3, 0x2A5A, 0x5EE5, 0x4F6C, 0x7DF7, 0x6C7E,
    0xA50A, 0xB483, 0x8618, 0x9791, 0xE32E, 0xF2A7, 0xC03C, 0xD1B5,
    0x2942, 0x38CB, 0x0A50, 0x1BD9, 0x6F66, 0x7EEF, 0x4C74, 0x5DFD,
    0xB58B, 0xA402, 0x9699, 0x8710, 0xF3AF, 0xE226, 0xD0BD, 0xC134,
    0x39C3, 0x284A, 0x1AD1, 0x0B58, 0x7FE7, 0x6E6E, 0x5CF5, 0x4D7C,
    0xC60C, 0xD785, 0xE51E, 0xF497, 0x8028, 0x91A1, 0xA33A, 0xB2B3,
    0x4A44, 0x5BCD, 0x6956, 0x78DF, 0x0C60, 0x1DE9, 0x2F72, 0x3EFB,
    0xD68D, 0xC704, 0xF59F, 0xE416, 0x90A9, 0x8120, 0xB3BB, 0xA232,
    0x5AC5, 0x4B4C, 0x79D7, 0x685E, 0x1CE1, 0x0D68, 0x3FF3, 0x2E7A,
    0xE70E, 0xF687, 0xC41C, 0xD595, 0xA12A, 0xB0A3, 0x8238, 0x93B1,
    0x6B46, 0x7ACF, 0x4854, 0x59DD, 0x2D62, 0x3CEB, 0x0E70, 0x1FF9,
    0xF78F, 0xE606, 0xD49D, 0xC514, 0xB1AB, 0xA022, 0x92B9, 0x8330,
    0x7BC7, 0x6A4E, 0x58D5, 0x495C, 0x3DE3, 0x2C6A, 0x1EF1, 0x0F78,
};

// ──────────────────────────────────────────────────────────────────────────

void HdlcCodec::reset()
{
    m_state = State::Searching;
    m_complete = false;
    m_aborted  = false;
    m_fcsValid = false;
    m_prevNrzi = 0;
    m_patDet   = 0;
    m_currentByte = 0;
    m_bitIndex    = 0;
    m_frameByteCount = 0;
    m_frameSizeBits  = 0;
    m_preambleCount  = 0;
    m_rawBitsInFrame = 0;
    m_bitsAfterFlag  = 0;
    m_actualFcs   = {};
    m_expectedFcs = {};
}

int HdlcCodec::bitstreamSize() const
{
    if (m_state != State::InFrame)
        return 0;
    return static_cast<int>(m_frameByteCount) * 8 + m_bitIndex;
}

void HdlcCodec::beginFrame()
{
    m_currentByte    = 0;
    m_bitIndex       = 0;
    m_frameByteCount = 0;
    m_rawBitsInFrame = 0;
    m_bitsAfterFlag  = 0;
    m_fcsValid       = false;
    m_aborted        = false;
}

void HdlcCodec::accumulateBit(uint8_t bit)
{
    if (bit)
        m_currentByte |= static_cast<uint8_t>(1u << m_bitIndex);
    ++m_bitIndex;

    if (m_bitIndex == 8) {
        if (m_frameByteCount < kMaxFrameBytes)
            m_frameBuffer[m_frameByteCount++] = m_currentByte;
        else {
            // Buffer overflow — abandon frame, return to searching
            m_state  = State::Searching;
            m_patDet = 0;
            beginFrame();
        }
        m_currentByte = 0;
        m_bitIndex    = 0;
    }
}

bool HdlcCodec::closeFrame()
{
    m_complete = true;
    m_state    = State::InPreamble;  // ready for next frame (shared flag)

    // A valid AX.25 frame body is always a whole number of bytes.  After the
    // last complete data byte (m_bitIndex==0), the 7 flag bits that precede
    // the closing flag zero accumulate in m_currentByte; m_bitIndex reaches 7.
    // Any other value means the frame wasn't byte-aligned → reject.
    if (m_bitIndex != 7 || m_frameByteCount < 2) {
        m_fcsValid = false;
        m_frameSizeBits = m_rawBitsInFrame;
        return true;
    }

    m_frameSizeBits = m_rawBitsInFrame;
    m_bitsAfterFlag = 0;  // ready for postamble flag detection in InPreamble

    // Compute expected FCS over frame[0..N-3] (all bytes except the 2 FCS bytes).
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i + 2 < m_frameByteCount; ++i)
        crc = (crc >> 8) ^ kCrcTable[(crc ^ m_frameBuffer[i]) & 0xFF];
    crc ^= 0xFFFF;

    m_expectedFcs[0] = static_cast<uint8_t>(crc & 0xFF);
    m_expectedFcs[1] = static_cast<uint8_t>((crc >> 8) & 0xFF);

    m_actualFcs[0] = m_frameBuffer[m_frameByteCount - 2];
    m_actualFcs[1] = m_frameBuffer[m_frameByteCount - 1];

    m_fcsValid = (m_actualFcs == m_expectedFcs);
    return true;
}

// ──────────────────────────────────────────────────────────────────────────

bool HdlcCodec::processBit(uint8_t nrziTone)
{
    // Deferred reset: clear complete/aborted flags at the start of the next call
    // so callers can read them after the frame-closing call returns.
    if (m_complete) {
        m_complete = false;
        m_aborted  = false;
    }

    // NRZI decode: no transition = 1 (mark held), transition = 0 (space).
    const uint8_t decoded = (nrziTone == m_prevNrzi) ? 1u : 0u;
    m_prevNrzi = nrziTone;

    // Shift new decoded bit into the rolling pattern register, newest at MSB.
    m_patDet = static_cast<uint8_t>((m_patDet >> 1) | (decoded << 7));

    switch (m_state) {

    // ── Searching: scan for the first preamble flag ─────────────────────
    case State::Searching:
        if (m_patDet == 0x7E) {
            m_state = State::InPreamble;
            m_preambleCount = 1;
            beginFrame();
        }
        break;

    // ── InPreamble: count consecutive flags, wait for first frame bit ───
    //
    // We require 8 consecutive non-flag bits before entering InFrame.
    // This mirrors libmodem's (bitstream_size >= 8) check and prevents a
    // false InFrame transition on the very first bit of the next preamble
    // flag (which would briefly disrupt pat_det away from 0x7E).
    //
    // Bits accumulate during this window; they become the first byte of the
    // frame.  A flag anywhere in the window calls beginFrame() and resets
    // the count, discarding those partial bits.
    case State::InPreamble:
        if (m_patDet == 0x7E) {
            // Another consecutive preamble flag.
            ++m_preambleCount;
            beginFrame();  // clears m_bitsAfterFlag, byte state, etc.
        } else {
            // Potential frame data — accumulate and count.
            ++m_rawBitsInFrame;
            ++m_bitsAfterFlag;
            accumulateBit(decoded);
            // accumulateBit may have set state = Searching on overflow;
            // only transition to InFrame if we're still in InPreamble.
            if (m_state == State::InPreamble && m_bitsAfterFlag >= 8)
                m_state = State::InFrame;
        }
        break;

    // ── InFrame: collect frame bytes ────────────────────────────────────
    case State::InFrame:
        ++m_rawBitsInFrame;  // first 8 bits counted in InPreamble; continue here

        // Priority 1 — Flag (checked before stuffing; 0x7E matches both but
        // flag must win so the closing zero is never discarded as a stuffed bit).
        if (m_patDet == 0x7E) {
            return closeFrame();
        }

        // Priority 2 — Bit stuffing: current bit is 0 (bit7=0) and the five
        // preceding bits are all 1 (bits 6..2 = 11111).  Discard the stuffed zero.
        if ((m_patDet >> 2) == 0x1F) {
            break;  // discard — do not accumulate
        }

        // Priority 3 — Abort: 7 consecutive ones (0xFE = 01111111 after final 0
        // closes the abort, or 0xFF for 8 ones).
        if (m_patDet == 0xFE || m_patDet == 0xFF) {
            m_aborted = true;
            m_state   = State::Searching;
            m_patDet  = 0;
            beginFrame();
            break;
        }

        // Data bit — accumulate LSB-first into current byte.
        accumulateBit(decoded);
        break;
    }

    return false;
}

} // namespace AetherSDR
