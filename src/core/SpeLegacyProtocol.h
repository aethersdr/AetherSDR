#pragma once

#include <functional>
#include <optional>

#include <QByteArray>

#include "SpeProtocol.h"

namespace AetherSDR {
namespace Spe {

// Original SPE Expert 1K-FA (the RS-232-only model that predates the
// 1.3K/1.5K/2K-FA). Authority: SPE "Expert 1K-FA Remote Control Protocol"
// Rev 2.0; see docs/architecture/spe-expert-amplifier-design.md §12. Not the
// later 1K-FA "Taurus", whose protocol is unknown here.
//
// Same sync framing as the newer family, different everything else:
//   host -> amp:  | 0x55 0x55 0x55 | CNT | DATA... | CHK |
//   amp  -> host: | 0xAA 0xAA 0xAA | CNT | DATA... | CHK |
// CHK = sum(DATA) mod 256 in both directions, no CR LF. The amplifier is
// silent until RCU_ON, then streams a 30-byte binary Status at 5..8 Hz.
// Fixed 9600 8N1, no handshake.
namespace Legacy {

constexpr quint8 kRcuOn  = 0x80;  // start the Status stream
constexpr quint8 kRcuOff = 0x81;  // stop it (answered with one Status)
constexpr quint8 kKeyOn  = 0x10;  // followed by one front-panel key code

constexpr quint8 kAck = 0x06;
constexpr quint8 kNak = 0x15;
constexpr quint8 kUnk = 0xFF;

constexpr quint8 kStatusDataLength = 30;

// Model ID the 1K-FA is reported under. The legacy Status carries no model
// field, so this is synthetic: it keys the 1K-FA row of Spe::modelSpec().
inline constexpr char kModelId[] = "10K";

QByteArray buildPacket(const QByteArray& data);
QByteArray buildRcuOn();
QByteArray buildRcuOff();
// Translates the shared Spe::Key to the 1K-FA's own key code. POWER maps to
// the 1K-FA's MODE key, which toggles HALF/FULL output.
QByteArray buildKeyCommand(Key key);
quint8 keyCode(Key key);

// One amplifier->host frame; `data` excludes sync, CNT and checksum.
struct Frame {
    QByteArray data;
    bool isReply() const { return data.size() == 1; }  // ACK / NAK / UNK
    bool isStatus() const { return data.size() == kStatusDataLength; }
};

// Byte-stream parser for the amplifier->host direction. Resyncs on the
// 0xAA x3 sync sequence after any checksum or length failure, so a corrupted
// byte costs one frame, not the stream.
class FrameParser {
public:
    void setFrameCallback(std::function<void(const Frame&)> cb) { m_onFrame = std::move(cb); }
    void feed(const QByteArray& bytes);
    void reset() { m_buf.clear(); }

private:
    QByteArray m_buf;
    std::function<void(const Frame&)> m_onFrame;
};

// Decodes the 30-byte Status payload into the shared Spe::Status so the SPE
// applet renders it unchanged. Mapping notes:
//   - id is kModelId; powerLevel is 'H' for FULL, 'L' for HALF.
//   - bandIndex is translated to the newer family's table (no 60m here).
//   - txAntenna is 1..4 (0 = none); atuState is not reported (null).
//   - STANDBY reports SWR directly; OPERATE reports PA gain instead, so
//     swrAnt is derived from forward/reverse power there and flagged
//     swrEstimated. swrAtu stays 0.
//   - Warnings come from the display context; alarmDetail flags the ALARM bit.
// Returns nullopt for a payload of the wrong size.
std::optional<Status> parseStatus(const QByteArray& payload);

// VSWR from forward/reverse power; 0 when there is no forward power, and
// capped at 99.9 for a reflection at or above the forward power.
float swrFromPower(float forwardW, float reverseW);

}  // namespace Legacy
}  // namespace Spe
}  // namespace AetherSDR
