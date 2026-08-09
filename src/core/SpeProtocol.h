#pragma once

#include <functional>
#include <optional>

#include <QByteArray>
#include <QChar>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace AetherSDR {

// SPE Expert linear amplifier serial protocol (1.3K-FA / 1.5K-FA / 2K-FA).
//
// Protocol authority: "Application Programmer's Guide — Expert 1.3K-FA,
// Expert 1.5K-FA, Expert 2K-FA", Rev 1.1 (2015-10-15), published by SPE
// s.r.l. (linear-amplifier.com) — the manufacturer's own spec (see
// docs/architecture/spe-expert-amplifier-design.md and THIRD_PARTY_LICENSES
// for the provenance record).
//
// Wire framing is asymmetric by direction (unlike ACOM's, which is
// symmetric — see AcomProtocol.h for that family):
//
//   host -> amp:  | 0x55 0x55 0x55 | CNT | DATA... | CHK |
//   amp  -> host: | 0xAA 0xAA 0xAA | CNT | DATA... | CHK... |
//
// CNT counts the DATA bytes only (checksum excluded). CHK is the modulo-256
// sum of the DATA bytes — for the single-byte commands this protocol
// actually uses, the checksum is simply the command byte repeated. The
// amplifier's replies use two different checksum shapes (spec §3/§5):
// the single-byte ACK echoes the received command with a 1-byte checksum,
// while the 67-byte Status string carries a 16-bit checksum (low byte =
// sum % 256, high byte = sum / 256) followed by CR LF.
//
// Every command is the equivalent of a front-panel keystroke; there is no
// richer command envelope. Complex operations (settings, antenna presets,
// firmware updates) are explicitly reserved by SPE for their own KTerm
// application and are out of scope here by design.
namespace Spe {

constexpr quint8 kHostSync = 0x55;  // host -> amplifier sync byte (x3)
constexpr quint8 kAmpSync  = 0xAA;  // amplifier -> host sync byte (x3)

// The Status string is fixed at 67 data characters (spec §5). Nothing the
// amplifier sends is longer, so any CNT beyond this small ceiling is noise
// (or a corrupted byte) rather than a real frame length.
constexpr quint8 kStatusDataLength = 67;
constexpr quint8 kMaxDataLength    = 72;

// Front-panel keystroke codes (spec §4). Supported from firmware
// 15_05_15_A_3s (2K-FA) / 15_05_15_A_15s (1.3K-FA) onward.
enum class Key : quint8 {
    Input      = 0x01,  // toggle input port 1/2
    BandDown   = 0x02,
    BandUp     = 0x03,
    Antenna    = 0x04,
    LMinus     = 0x05,  // manual ATU inductance — not exposed in v1 (KTerm territory)
    LPlus      = 0x06,
    CMinus     = 0x07,  // manual ATU capacitance — not exposed in v1
    CPlus      = 0x08,
    Tune       = 0x09,
    SwitchOff  = 0x0A,  // powers the amplifier down
    Power      = 0x0B,  // cycles the output power level (see Status::powerLevel)
    Display    = 0x0C,
    Operate    = 0x0D,  // toggles STANDBY <-> OPERATE
    Cat        = 0x0E,
    LeftArrow  = 0x0F,
    RightArrow = 0x10,
    Set        = 0x11,
};

constexpr quint8 kBacklightOn    = 0x82;
constexpr quint8 kBacklightOff   = 0x83;
constexpr quint8 kStatusRequest  = 0x90;

// ── Framing (host -> amp) ────────────────────────────────────────────────

// Builds a complete host->amp packet: 0x55 x3, CNT, data..., checksum,
// CR LF. The trailing CR LF is not in the spec's packet diagram, but the
// spec's own Node-RED-era reference traffic and the working Python
// implementation this module was validated against both required it on
// real 1.5K-FA hardware (commands without it were intermittently ignored
// over a ser2net link). Two extra bytes outside the framed packet are
// harmless to a parser keyed on the sync sequence, so they are always sent.
QByteArray buildPacket(const QByteArray& data);

QByteArray buildKeyCommand(Key key);
QByteArray buildStatusRequest();
QByteArray buildBacklightCommand(bool on);

// ── Framing (amp -> host) ────────────────────────────────────────────────

// A decoded amplifier->host frame. `data` excludes sync/CNT/checksum.
// The single-byte ACK (CNT == 1, 1-byte checksum) and the Status string
// (CNT == 67, 2-byte checksum + CR LF) are the only reply shapes the spec
// defines; both surface here and the caller dispatches on data size.
struct Frame {
    QByteArray data;
    bool isAck() const { return data.size() == 1; }
};

// Streaming byte-oriented parser for the amplifier->host direction. Feed it
// raw bytes from any transport (QSerialPort or QTcpSocket — same bytes
// either way); it resyncs on the 0xAA 0xAA 0xAA sync sequence whenever a
// candidate frame fails validation, so garbage or partial data self-heals
// rather than desyncing permanently. Deliberately has zero Qt-networking
// dependency so it's unit-testable against captured byte sequences.
class FrameParser {
public:
    void setFrameCallback(std::function<void(const Frame&)> cb) { m_onFrame = std::move(cb); }
    void feed(const QByteArray& bytes);
    void reset() { m_buf.clear(); }

private:
    // Drops the rejected sync candidate at buffer position 0 and shifts to
    // the next 0xAA in a single pass. Returns false and empties the buffer
    // when no further sync byte remains.
    bool resyncToNextSync();

    QByteArray m_buf;
    std::function<void(const Frame&)> m_onFrame;
};

// ── Status string decode (spec §5) ───────────────────────────────────────

// The 67-character Status payload is ASCII CSV: a leading marker character
// ("C" on all firmware observed), then 19 fixed-length fields. All values
// decoded to native types; raw characters kept where the spec enumerates
// single-letter codes.
struct Status {
    QString id;              // "13K" (1.3K-FA), "15K" (1.5K-FA), "20K" (2K-FA)
    bool    operate{false};      // O = OPERATE, S = STANDBY
    bool    transmitting{false}; // T = TX, R = RX
    QChar   bank{u'x'};          // A/B on 1.3K/1.5K-FA; 2K-FA always reads x
    int     input{1};            // selected input port, 1 or 2
    int     bandIndex{0};        // 00 (160m) .. 11 (4m)
    int     txAntenna{0};        // 0..4 (1.3K/1.5K) or 0..6 (2K)
    QChar   atuState{};          // t = tunable, b = ATU bypassed, a = ATU enabled
    QString rxAntenna;           // "0r" when no RX-only antenna is set
    QChar   powerLevel{};        // L / M / H
    float   outputPowerW{0.0f};  // 0 in RX
    float   swrAtu{0.0f};        // VSWR before the ATU (TX only)
    float   swrAnt{0.0f};        // VSWR at the antenna (TX only)
    float   paVoltageV{0.0f};    // supply voltage (0.0 in RX)
    float   paCurrentA{0.0f};    // absorbed current on TX
    int     tempUpper{0};        // heatsink degC/degF (upper on 2K-FA)
    int     tempLower{0};        // 2K-FA lower heatsink; 1.3K-FA always 0
    int     tempCombiner{0};     // 2K-FA combiner; 1.3K-FA always 0
    QChar   warning{u'N'};       // N = none; see warningText()
    QChar   alarm{u'N'};         // N = none; see alarmText()
};

// Decodes a Status frame's 67-character payload. Tolerates the payload
// arriving with or without the leading marker character (19 or 20
// comma-separated tokens), so a firmware that drops the marker still
// parses. Returns nullopt when the field count is short.
std::optional<Status> parseStatus(const QByteArray& payload);

// Band name for Status::bandIndex — 00 (160m) through 11 (4m); the 2K-FA
// tops out at 10 (6m). Out-of-range indices return "?m".
QString bandName(int index);

// Human-readable text for the WARNINGS / ALARMS single-letter codes
// (spec §5's two tables). Unknown codes echo the raw letter rather than
// pretending to know.
QString warningText(QChar code);
QString alarmText(QChar code);

QString powerLevelName(QChar code);  // L/M/H -> LOW/MID/HIGH

// ── Remote power-ON (RFC 2217 Telnet COM-port control) ───────────────────

// The Expert powers ON via a pulse on a hardware line of its serial
// connector — not a protocol command. Over a ser2net telnet-mode proxy that
// pulse is expressible with RFC 2217 SET-CONTROL messages driving the
// proxy's DTR/RTS lines. The exact sequence below (500 ms after announcing
// WILL COM-PORT-OPTION: DTR on, 100 ms, DTR off + RTS on, 1000 ms, DTR on +
// RTS off) is carried verbatim from the field-proven reference application
// for the 1.5K-FA — see the design note §4. SpeConnection owns the timing;
// these helpers just build the byte sequences (kept here so they are
// unit-testable against RFC 2217's literal framing).
namespace Rfc2217 {

constexpr quint8 kDtrOn  = 0x08;
constexpr quint8 kDtrOff = 0x09;
constexpr quint8 kRtsOn  = 0x0B;
constexpr quint8 kRtsOff = 0x0C;

// IAC WILL COM-PORT-OPTION — asks the proxy to interpret RFC 2217 frames.
QByteArray buildWillComPortOption();
// IAC SB COM-PORT-OPTION SET-CONTROL <ctrl> IAC SE
QByteArray buildSetControl(quint8 ctrl);

// The peer's answer to buildWillComPortOption(). ser2net replies
// IAC DO COM-PORT-OPTION when the port is configured
// `accepter: telnet(rfc2217=true),<port>`, and IAC DONT for a plain-telnet
// port. A raw port never answers at all — it forwards the negotiation into
// the amplifier's UART as payload, where it lands as junk. Distinguishing
// those three is the difference between driving the power line and silently
// doing nothing, so powerOn() reads this rather than assuming success.
enum class OptionReply { None, Accepted, Refused };

// Scans a raw inbound chunk for that answer, returning the last one present.
// Read-only: the negotiation bytes stay in the stream for FrameParser, whose
// sync-run resync steps over them. A false positive would need the literal
// sequence FF FD 2C inside a Status payload, which is ASCII CSV and cannot
// contain 0xFF.
OptionReply scanComPortOptionReply(const QByteArray& bytes);

}  // namespace Rfc2217

// ── Per-model display scaling ────────────────────────────────────────────

// The Status ID field identifies the model directly on every response, so —
// unlike the ACOM family — no auto-ranging heuristics are needed; the gauge
// scale simply follows the reported ID.
struct ModelSpec {
    QString id;            // Status ID value this spec matches
    QString displayName;   // "1.5K-FA"
    float   nominalPowerW{0};  // rated output at HIGH — gauge red threshold
    float   maxPowerW{0};      // gauge ceiling at HIGH
    float   warnPowerW{0};     // gauge yellow zone start at HIGH
    float   lowNominalW{0};    // rated output at the LOW power level
    float   midNominalW{0};    // rated output at the MID power level
    bool    hasMemoryBanks{false};  // A/B bank field is meaningful
    bool    hasCombiner{false};     // lower/combiner temperatures are real
};

// 1.5K-FA is the entry validated against real hardware by this project
// (its gauge thresholds mirror a field-proven control application for that
// model). 1.3K-FA and 2K-FA figures come from SPE's published rated output
// with the same +100 W gauge-ceiling convention. Unknown IDs fall back to
// the 1.5K-FA entry.
const ModelSpec& modelSpec(const QString& id);
QStringList modelIds();

// Rated output for the currently selected power level (Status::powerLevel).
// H (or an unknown letter) is the model's full nominalPowerW.
float levelNominalW(const ModelSpec& spec, QChar level);

// The complete power-gauge axis for that level, so the bar rescales as the
// operator cycles LOW/MID/HIGH exactly like the amplifier's own display.
//
// At HIGH this returns the model row's own three figures verbatim — the
// table is the single source of truth, so correcting a row (the design note
// §5 invites an owner to verify the 1.3K-FA/2K-FA numbers against real
// hardware) actually moves the bar instead of being silently overridden by
// a duplicate derivation in the GUI wiring. LOW/MID have no tabulated
// warn/max, so they take the hardware-validated 1.5K-FA shape: yellow from
// nominal−50 W, red from nominal, ceiling at nominal+100 W — which is
// exactly what the tabulated HIGH rows encode too.
struct GaugeRange {
    float nominalW{0};  // gauge red threshold
    float warnW{0};     // gauge yellow zone start
    float maxW{0};      // gauge ceiling
};
GaugeRange levelGaugeRange(const ModelSpec& spec, QChar level);

}  // namespace Spe
}  // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::Spe::Status)
