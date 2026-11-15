#pragma once

#include <QByteArray>
#include <QVector>
#include <QtGlobal>

// Pure KISS (Keep It Simple Stupid) TNC framing — no Qt::Network dependency so
// it can be unit-tested standalone. KISS wraps raw AX.25 frames (address through
// info, WITHOUT the HDLC flags or FCS) for exchange with a host APRS/packet
// application over a serial or TCP link.
//
// Reference: the KISS protocol (Chepponis & Karn, 1987).

namespace AetherSDR::kiss {

constexpr quint8 kFend = 0xC0;  // Frame End delimiter
constexpr quint8 kFesc = 0xDB;  // Frame Escape
constexpr quint8 kTfend = 0xDC; // Transposed Frame End
constexpr quint8 kTfesc = 0xDD; // Transposed Frame Escape

// Low-nibble command codes of the KISS type byte (high nibble = port).
constexpr quint8 kCmdData = 0x00;
constexpr quint8 kCmdTxDelay = 0x01;
constexpr quint8 kCmdPersistence = 0x02;
constexpr quint8 kCmdSlotTime = 0x03;
constexpr quint8 kCmdTxTail = 0x04;
constexpr quint8 kCmdFullDuplex = 0x05;
constexpr quint8 kCmdSetHardware = 0x06;
constexpr quint8 kCmdReturn = 0xFF;

// Largest in-progress KISS frame we will buffer before treating the stream as
// garbage and resyncing. A full AX.25 frame is well under 512 bytes; this is a
// generous guard against a runaway/desynced client.
constexpr int kMaxFrameBytes = 4096;

// Escape FEND/FESC occurrences in a payload per the KISS transposition rules.
QByteArray escape(const QByteArray& in);

// Reverse of escape(). Lone/!invalid escapes are passed through literally.
QByteArray unescape(const QByteArray& in);

// Wrap a raw AX.25 frame (no FCS) as a complete KISS data frame:
//   FEND, (port<<4 | CmdData), <escaped bytes>, FEND
QByteArray encodeDataFrame(const QByteArray& ax25NoFcs, quint8 port = 0);

// Incremental, resync-safe KISS deframer. Feed arbitrary byte chunks from a
// socket; returns any complete frames. Each returned frame includes its leading
// type byte (so callers can read the port/command) and is already unescaped.
class Decoder {
public:
    QVector<QByteArray> feed(const QByteArray& bytes);
    void reset();

    // True once we have seen our first FEND and are tracking a frame boundary.
    bool synced() const { return m_synced; }

private:
    QByteArray m_frame;
    bool m_synced = false;
    bool m_overflow = false;
};

// Convenience: split a complete (unescaped) KISS frame into its command nibble,
// port, and payload (everything after the type byte). Returns false if empty.
bool splitTypeByte(const QByteArray& frameWithType, quint8& port, quint8& command,
                   QByteArray& payload);

} // namespace AetherSDR::kiss
