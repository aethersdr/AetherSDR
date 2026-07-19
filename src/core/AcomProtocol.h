#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace AetherSDR {

// ACOM S-series (500S/600S/700S/1200S/1400S/2020S) RS-232 amplifier protocol.
//
// Protocol authority: "RF Amplifier ACOM 600S Serial Port Communication
// Protocol", v1.1 (2014-12-04), eng. Nikolay Nenov — the manufacturer's own
// published spec (see docs/architecture/acom-600s-amplifier-design.md and
// THIRD_PARTY_LICENSES for the full provenance record). Framing is identical
// across the whole S-series; only power-scaling constants and PAM2-field
// presence differ per model (see AcomModelSpec below).
//
// Wire framing (both directions):
//   | 0x55 | Address | Length | ...Data... | Checksum |
// Length counts every byte in the frame including the start byte and the
// checksum itself. Checksum = 256 - (sum of all prior bytes & 0xFF); a
// valid frame's full byte sum is 0 mod 256.
namespace Acom {

constexpr quint8 kStartByte = 0x55;

// The spec caps every message at 72 bytes; anything claiming to be larger
// is noise (or a bit-flip) rather than a real frame length.
constexpr quint8 kMaxFrameLength = 72;

// Message addresses this implementation understands. The spec defines many
// more (CAT passthrough config, manual band/antenna override, buzzer, LOG
// dump, service-mode tests, factory reset) — deliberately not implemented;
// see docs/architecture/acom-600s-amplifier-design.md §4 for why.
enum class Address : quint8 {
    SystemConfig     = 0x11,  // amp -> host, PCB versions/amplifier type/serial — one-shot, on request only
    Telemetry        = 0x2F,  // amp -> host, 72 B unified status
    ErrorCodes       = 0x21,  // amp -> host, 10-word fault bitmask
    RequestMessage   = 0x02,  // host -> amp, "send me message N" (used to request SystemConfig)
    Command          = 0x81,  // host -> amp, mode-change / clear-faults envelope
    TelemetryDisable = 0x91,  // host -> amp
    TelemetryEnable  = 0x92,  // host -> amp
    Ack              = 0x86,  // either direction, per-message acknowledgement
};

// Amplifier operating mode, decoded from the telemetry frame's mode byte
// high nibble (Byte 3, upper nibble). Values below 0x1..0xA per spec §3.4.8.
enum class Mode : quint8 {
    Reset      = 0x1,
    Init       = 0x2,
    Debug      = 0x3,
    Service    = 0x4,
    Standby    = 0x5,
    OperateRx  = 0x6,
    OperateTx  = 0x7,
    Atac       = 0x8,  // antenna-tune/change procedure in progress
    SetParams  = 0x9,
    PowerOff   = 0xA,
    Unknown    = 0x0,
};

// Mode-change targets accepted by Command sub-command 0x02 (spec §5.1).
enum class ModeCommand : quint8 {
    Standby  = 0x05,
    Operate  = 0x06,
    PowerOff = 0x0A,
};

QString modeName(Mode mode);

// ── Framing ──────────────────────────────────────────────────────────────

// Checksum over every byte of the frame EXCLUDING the checksum byte itself.
// A complete, valid frame (checksum byte included) sums to 0 mod 256.
quint8 checksum(const QByteArray& frameWithoutChecksum);

// Builds a complete, checksummed frame: 0x55, address, length, data..., checksum.
QByteArray buildFrame(quint8 address, const QByteArray& data = {});

// A single decoded, checksum-valid frame.
struct Frame {
    quint8     address{0};
    QByteArray data;  // payload only — excludes start/address/length/checksum
};

// Streaming byte-oriented parser. Feed it raw bytes from any transport
// (QSerialPort or QTcpSocket — the wire format is identical either way);
// it resyncs on the 0x55 start byte whenever a candidate frame's checksum
// fails, so garbage/partial data from a flaky link self-heals rather than
// desyncing permanently. Deliberately has zero Qt-networking dependency so
// it's unit-testable against captured byte sequences without a live device.
class FrameParser {
public:
    void setFrameCallback(std::function<void(const Frame&)> cb) { m_onFrame = std::move(cb); }
    void feed(const QByteArray& bytes);
    void reset() { m_buf.clear(); }

private:
    // Called with a rejected start byte (bad length or bad checksum) at buffer
    // position 0: drops from it to the next 0x55 in a single shift, so a run of
    // 0x55 noise resyncs in one O(n) pass rather than O(n^2) (a byte-at-a-time
    // drop shifts the whole buffer per byte). Returns false and empties the
    // buffer when no further start byte remains.
    bool resyncToNextStart();

    QByteArray m_buf;
    std::function<void(const Frame&)> m_onFrame;
};

// ── Telemetry decode (address 0x2F, 72 bytes) ───────────────────────────

// Raw wire values, exactly as the spec defines each field — no per-model
// scaling or unit conversion applied here (that needs AcomModelSpec, which
// this layer has no knowledge of). See AmpModel/GUI layer for the
// model-aware interpretation (temperature offset, gauge nominal/max).
struct Telemetry {
    Mode    mode{Mode::Unknown};

    quint16 dcPowerPam1_x10W{0};      // Byte 8-9,   [10 x W]
    quint32 systemClockSec{0};        // Byte 12-15, [s] — a cumulative total-
                                       //   operating-time counter, NOT time
                                       //   since the last power-on; confirmed
                                       //   against a real 600S (front-panel
                                       //   127:43:17 H:MM:SS matched the
                                       //   decoded 459,797s exactly, ruling
                                       //   out a high/low word swap too)
    quint16 paTempRaw{0};             // Byte 16-17, raw sensor units (subtract the
                                       //   model's TemperatureOffset for real degC)
    quint16 inputPower_x10W{0};       // Byte 20-21, [10 x W]
    quint16 forwardPowerW{0};         // Byte 22-23, [W]
    quint16 reflectedPowerW{0};       // Byte 24-25, [W]
    quint16 swr_x100{0};              // Byte 26-27, [100x] — divide by 100 for ratio
    quint16 dissipationPam1_x10W{0};  // Byte 28-29, [10 x W]
    quint16 vcc5_mV{0};               // Byte 36-37, [mV]
    quint16 vcc26_x10V{0};            // Byte 38-39, [10 x V]
    quint16 hv1_x10V{0};              // Byte 40-41, [10 x V]
    quint16 id1_mA{0};                // Byte 44-45, [mA] — PA drain current
    quint16 carrierFreqKHz{0};        // Byte 48-49, [kHz] — amp's own frequency counter

    quint8  fanSpeed{0};              // Byte 69 high nibble, 0..4
    quint8  activeBand{0};            // Byte 69 low nibble — index into bandName()

    quint8  errorCode{0xFF};          // Byte 66 — 0xFF means no active alarm
    quint16 errorParam{0};            // Byte 67-68
};

// Decodes a 0x2F Telemetry frame's payload (Frame::data, i.e. bytes 3..70 of
// the full 72-byte message — start/address/length/checksum already stripped
// by FrameParser). Returns nullopt if the payload is short.
std::optional<Telemetry> decodeTelemetry(const QByteArray& payload);

// LPF band name for Telemetry::activeBand (spec §3.3.7 / the 4-bit "Active
// FLP channel" field in the telemetry frame). Index 0 and 11-15 are reserved
// ("?m" in the amp's own display convention).
QString bandName(int index);

// Human-readable name for Telemetry::errorCode (Byte 66 of 0x2F — the
// single currently-displayed alarm condition). This is a small, verified
// subset (cross-checked against both the spec's bit-name table in message
// 0x21 and a working open-source reference client — see
// docs/architecture/acom-600s-amplifier-design.md and THIRD_PARTY_LICENSES)
// covering the faults an operator is actually likely to see. The full 0x21
// message carries ~80 independently-named bits across 10 words for a
// complete fault dashboard; that full table is a documented follow-up, not
// transcribed here yet — see the design doc's provenance notes on why a
// partial, verified table beats a large, unverified one.
QString errorCodeName(quint8 code);

// ── System config decode (address 0x11, one-shot on request) ────────────

// PCB versions / amplifier type / serial number. Not pushed automatically
// like Telemetry — the host must ask for it (see buildRequestMessage).
// Amplifier type is the only field this project uses programmatically
// (model auto-detection, §6 of the design doc); firmware/hardware versions
// and serial number are decoded because they're free once the frame is
// being parsed anyway, useful for diagnostics and for reporting a new type
// code back to the project.
struct SystemConfig {
    quint8  amplifierType{0};      // Byte 3 — spec confirms only "1 = A600S"
    quint8  fwVersion{0};          // Byte 4
    quint8  fwSubVersion{0};       // Byte 5
    QString serialNumberHex;       // Byte 16-27 (12 bytes) — exact ASCII vs binary
                                    // encoding is unconfirmed, so this is a hex
                                    // dump (safe regardless of true encoding),
                                    // not a claimed human-readable serial string.
    quint8  hardFaultCount{0};     // Byte 28 — count of stored Hard Fault records
};

// Decodes a 0x11 SystemConfig frame's payload. Returns nullopt if short.
std::optional<SystemConfig> decodeSystemConfig(const QByteArray& payload);

// Confident model name for a SystemConfig::amplifierType value, or an empty
// string if unrecognized. Only type 1 is confirmed by the spec — see the
// design doc §6 for why the rest of the S-series isn't (and can't yet be)
// enumerated here, and how auto-ranging covers the gap.
QString modelNameForAmplifierType(quint8 amplifierType);

// ── Commands (host -> amp) ───────────────────────────────────────────────

QByteArray buildModeCommand(ModeCommand cmd);
QByteArray buildClearFaultsCommand();
QByteArray buildTelemetryEnable();
QByteArray buildTelemetryDisable();
QByteArray buildAck(quint8 receivedMessageNumber);
// Requests the amp send a specific one-shot message (e.g. SystemConfig).
// Spec §3.2. Not needed for Telemetry/ErrorCodes, which push automatically
// once telemetry is enabled.
QByteArray buildRequestMessage(quint8 desiredAddress);

// ── Per-model scaling (spec is shared; these constants are per amplifier) ──

struct ModelSpec {
    QString name;
    float   nominalForwardW{0};
    float   maxForwardW{0};
    float   nominalReflectedW{0};
    float   maxReflectedW{0};
    int     temperatureOffset{0};  // subtract from Telemetry::paTempRaw for degC
    bool    hasPam2{false};
};

// 600S is the only entry verified against real hardware by this project.
// The rest are derived from ACOM's own published rated-output specs (not
// the reference client's constants, which turned out to be wrong for the
// higher-power models — see design doc §6) using a documented, but still
// unverified-beyond-600S, methodology. Treat as best-effort until confirmed.
const ModelSpec& modelSpec(const QString& modelName);
QStringList modelNames();

// Model tiers in ascending power order — the order auto-ranging steps
// through. Same ordering by nominal and by max power, so one list serves
// both.
QStringList orderedModelTiers();

// Smallest tier whose maxForwardW comfortably fits an observed forward-power
// reading (small margin to avoid boundary flapping at the exact ceiling),
// clamped to the top tier if the reading exceeds even that. Pure/stateless —
// AcomConnection owns deciding *when* to act on this (only moving up, never
// back down mid-session; see design doc §6).
QString tierForForwardPower(float watts);

}  // namespace Acom
}  // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::Acom::Telemetry)
Q_DECLARE_METATYPE(AetherSDR::Acom::SystemConfig)
