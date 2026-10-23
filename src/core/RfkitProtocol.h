#pragma once

#include <optional>

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QVector>

namespace AetherSDR {

// RF-KIT amplifier REST API codec.
//
// Protocol authority: the vendor's own published "RFKIT API and SDK
// Documentation" (OpenAPI document, version 0.9.0), a public manufacturer
// document, so this is a clean-room input under constitution Principle IV.
// See docs/architecture/rfkit-amplifier-design.md for the design note.
//
// Unlike every other amplifier peripheral here the wire is plain HTTP/1.1
// carrying JSON, not a socket protocol with its own framing, so this layer is
// stateless: one parse function per endpoint (bytes in, optional struct out)
// and one body builder per writable endpoint. RfkitConnection owns the
// polling, the request queue and the HTTP plumbing.
//
//   GET  /info                   device identity + firmware versions
//   GET  /power                  forward / reflected / SWR / temp / volts / amps
//   GET  /data                   band, frequency, free-text status
//   GET  /tuner                  tuner mode + L/C network (read-only)
//   GET  /antennas               every antenna with ACTIVE/AVAILABLE/DISABLED
//   GET  /antennas/active        the active antenna
//   PUT  /antennas/active        select an antenna
//   GET  /operate-mode           OPERATE | STANDBY
//   PUT  /operate-mode           set OPERATE | STANDBY
//   GET  /operational-interface  UNIV | CAT | UDP | TCI (+ read-only error)
//   POST /error/reset            clear the amplifier's error state
//
// Everything parsed here arrives from a LAN peer, so constitution Principle
// VII applies: bodies are size-capped, a document that is not the expected
// JSON shape yields nullopt rather than a half-filled struct, and a
// control-state enum (operate mode, operational interface, antenna type)
// that this codec cannot interpret is refused rather than guessed. Fields the
// document marks descriptive only (units, max_value, tuner L/C) default
// quietly when absent, so a firmware that omits one does not blank the applet.
namespace Rfkit {

// The API document's examples use port 8080 (http://localhost:8080/...). The
// Peripherals row exposes it as an editable field.
inline constexpr quint16 kDefaultPort = 8080;

// No endpoint answers with more than a few hundred bytes. A peer that sends
// more is not an RF-KIT, so the body is dropped rather than parsed.
inline constexpr int kMaxBodyBytes = 64 * 1024;

inline constexpr int kMinInternalAntenna = 1;
inline constexpr int kMaxInternalAntenna = 4;
// The external antenna is a single selection. The API document gives
// ExternalAntenna.number (1-16) as readOnly: it names the port of the
// operator's external antenna switch, which the amplifier reports but cannot
// be told to change, so the selection request carries no number. An external
// Antenna therefore always has number 0; the port the amplifier reports is kept
// separately, as AntennaWithState::externalPort. A real RF2K-S (firmware
// G200C267) lists {"type":"EXTERNAL","state":"AVAILABLE"} with no number while
// it is not active; what it reports while it is active has not been observed.
inline constexpr int kExternalAntennaNumber = 0;
inline constexpr int kMinExternalPort = 1;
inline constexpr int kMaxExternalPort = 16;

// Endpoint paths, relative to the device root.
inline constexpr char kPathInfo[]        = "/info";
inline constexpr char kPathPower[]       = "/power";
inline constexpr char kPathData[]        = "/data";
inline constexpr char kPathTuner[]       = "/tuner";
inline constexpr char kPathAntennas[]    = "/antennas";
inline constexpr char kPathAntennaActive[] = "/antennas/active";
inline constexpr char kPathOperateMode[] = "/operate-mode";
inline constexpr char kPathInterface[]   = "/operational-interface";
inline constexpr char kPathErrorReset[]  = "/error/reset";

// ── Value types ──────────────────────────────────────────────────────────
// The API wraps every measurement as {value, unit[, max_value]}. The unit is
// carried through so the applet shows what the device said instead of
// assuming one.

struct IntValue {
    int value{0};
    QString unit;
};

struct FloatValue {
    float value{0.0f};
    QString unit;
};

// maxValue is the device's reported max_value (the present power limit, not a
// meter range); parsed for completeness, not used for scaling. 0 means "not
// reported".
struct IntWithMax {
    int value{0};
    int maxValue{0};
    QString unit;
};

struct FloatWithMax {
    float value{0.0f};
    float maxValue{0.0f};
    QString unit;
};

// ── Responses ────────────────────────────────────────────────────────────

struct Info {
    QString device;
    QString customName;  // the operator-assigned name; may be empty
    int guiVersion{0};
    int controllerVersion{0};
};

struct Power {
    FloatValue temperature;
    FloatValue voltage;
    FloatValue current;
    IntWithMax forward;
    IntWithMax reflected;
    FloatWithMax swr;
};

struct Data {
    IntValue band;
    IntValue frequency;
    QString status;  // free text; the API document does not enumerate it
};

enum class TunerMode { Unknown, Bypass, Manual, AutoTuning, Auto, Off };
enum class TunerSetup { Unknown, NotTuned, Bypass, CL, LC };

struct Tuner {
    TunerMode mode{TunerMode::Unknown};
    TunerSetup setup{TunerSetup::Unknown};
    std::optional<IntValue> l;
    std::optional<IntValue> c;
    std::optional<IntValue> tunedFrequency;
    std::optional<IntValue> segmentSize;
};

enum class OperateMode { Standby, Operate };

enum class OperationalInterface { Univ, Cat, Udp, Tci };

struct InterfaceState {
    // Not named "interface": that is a macro in the Windows SDK headers.
    OperationalInterface active{OperationalInterface::Univ};
    QString error;  // read-only; empty when the device reported none
};

enum class AntennaType { Internal, External };

struct Antenna {
    AntennaType type{AntennaType::Internal};
    int number{1};  // 1-4 for an internal antenna; always 0 for the external one

    bool operator==(const Antenna& other) const
    {
        return type == other.type && number == other.number;
    }
};

enum class AntennaState { Active, Available, Disabled };

struct AntennaWithState {
    Antenna antenna;
    AntennaState state{AntennaState::Available};
    // External entry only: the external switch port the amplifier reported
    // (1-16), or 0 when it sent none or one outside that range.
    int externalPort{0};
};

// The device's error body, sent with a 400 or 409: {"status":..,"reason":..}.
struct ApiError {
    QString status;
    QString reason;
};

// One snapshot of everything the connection has learned. An unset optional
// means "the device has not answered that endpoint yet", which is not the
// same as a reading of zero, and the applet renders it as "—".
struct Status {
    std::optional<Info> info;
    std::optional<Power> power;
    std::optional<Data> data;
    std::optional<Tuner> tuner;
    std::optional<OperateMode> operate;
    std::optional<InterfaceState> interfaceState;
    std::optional<QVector<AntennaWithState>> antennas;
};

// ── Parsers ──────────────────────────────────────────────────────────────
// Each takes the raw HTTP body. nullopt means the body was oversized, not
// JSON, not the expected shape, or carried a control value this codec cannot
// interpret.

std::optional<Info> parseInfo(const QByteArray& body);
std::optional<Power> parsePower(const QByteArray& body);
std::optional<Data> parseData(const QByteArray& body);
std::optional<Tuner> parseTuner(const QByteArray& body);
std::optional<OperateMode> parseOperateMode(const QByteArray& body);
std::optional<InterfaceState> parseInterface(const QByteArray& body);
std::optional<Antenna> parseAntenna(const QByteArray& body);
// The /antennas list: a bare array, or the {"antennas":[...]} object a real
// RF2K-S sends. Entries that are malformed are skipped; the call fails only if
// the document is neither.
std::optional<QVector<AntennaWithState>> parseAntennas(const QByteArray& body);
std::optional<ApiError> parseApiError(const QByteArray& body);

// ── Request bodies ───────────────────────────────────────────────────────
// Compact JSON. buildAntennaBody returns an empty array for an antenna the
// device cannot have (an internal number outside 1-4, an external number other
// than 0), so such a request never reaches the wire. The external body carries
// no number, because the API document marks ExternalAntenna.number readOnly.

QByteArray buildOperateModeBody(OperateMode mode);
QByteArray buildAntennaBody(const Antenna& antenna);

// ── Presentation helpers ─────────────────────────────────────────────────

// Temperature in degrees Celsius, converting when the device reports °F.
float temperatureCelsius(const FloatValue& temperature);
// "14.200 MHz" from a frequency in Hz, kHz or MHz; "<value> <unit>" for any
// other unit, so an unexpected unit is shown rather than mis-scaled.
QString formatFrequency(const IntValue& frequency);
// "20 m": the value and the unit the device gave it.
QString formatBand(const IntValue& band);
// "1".."4" for the internal antennas, "EXT" for the external one.
QString antennaLabel(const Antenna& antenna);
QString tunerModeLabel(TunerMode mode);
QString tunerSetupLabel(TunerSetup setup);
QString interfaceLabel(OperationalInterface iface);

}  // namespace Rfkit
}  // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::Rfkit::Status)
