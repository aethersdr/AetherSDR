#pragma once

#include <QByteArray>
#include <QString>

#include <array>
#include <cstdint>
#include <vector>

namespace AetherSDR::ctr2hid {

// CTR2 USB link, wire format version 0, carried in 8-byte HID reports
// (report ID 0x01, both directions). A Report here is the 8 bytes after the
// report ID. Full specification and test vectors:
// docs/ctr2-usb-relay-design.md ("USB link specification").
//
// Header: [0xFF][version][counter][type][packets hi][packets lo]
//         [length hi][length lo]      packets includes the header
// Data:   [counter][7 payload bytes, zero-padded after the last]
//
// Counters are 7-bit (0x00-0x7F, wrapping 0x7F -> 0x00), so a data report
// never starts with the 0xFF header marker.
//
// Extension types 0x40-0x5F are negotiated per device through HID Feature
// report 0x02 (see Capabilities below). They are sequenced like Data, and a
// receiver accepts only the ones negotiated; a CTR2 that offers none never
// sees them.
constexpr int kReportBytes = 8;
constexpr int kDataBytesPerReport = 7;
constexpr std::uint8_t kReportId = 0x01;
constexpr std::uint8_t kMarker = 0xFF;
constexpr std::uint8_t kVersion = 0x00;
constexpr std::uint8_t kCounterMask = 0x7F;
constexpr int kMaxPayloadBytes = 512;      // DATA
constexpr int kMaxDatagramBytes = 1472;    // one UDP datagram, excluding the port
constexpr int kMaxMessageBytes = 2 + kMaxDatagramBytes;

using Report = std::array<std::uint8_t, kReportBytes>;

enum class MessageType : std::uint8_t {
    Data = 0x00,    // opaque radio TCP bytes, either direction
    Hello = 0x01,   // host: calling the device; device: restart the link
    Ready = 0x02,   // device: answering HELLO; host: radio connection open
    Closed = 0x03,  // either direction: link ended; device restarts with Hello
    Datagram = 0x04,  // one UDP datagram: [radio port hi][radio port lo][bytes]
    // Extensions, only where negotiated:
    AudioSpectrum = 0x40,  // host -> device: [bars N][low Hz][span Hz][N levels]
    RelayUdpPort = 0x41,   // host -> device: [port hi][port lo], this link's UDP endpoint
    AudioSpectrumLinear = 0x42,  // host -> device: [bars N][low Hz][high Hz][N levels], even split
    CwText = 0x43,         // host -> device: [confidence 0..3][decoded CW, ASCII]
    Firmware = 0x44,       // both: an application update session (see firmware below)
};

constexpr std::uint8_t kExtensionFirst = 0x40;
constexpr std::uint8_t kExtensionLast = 0x5F;
constexpr std::uint32_t capabilityBit(MessageType type)
{
    return 1u << (static_cast<std::uint8_t>(type) - kExtensionFirst);
}
// Largest payload each extension type may carry; 0 for one not defined.
int extensionMaxLength(MessageType type);

// Wide reports: capability bit 5, which names no message (0x45 is never
// sent). Once accepted, both directions carry the link's 8-byte reports
// bundled in HID report 0x04 instead of report 0x01, up to seven at a time:
//   [count 1..7][count reports of 8 bytes, back to back][zero padding]
// in 63 bytes after the report ID. The messages themselves do not change.
// A device offering it declares report 0x04 (Input and Output) beside report
// 0x01, which makes Windows pad every report read from it to 64 bytes.
constexpr std::uint32_t kWideReportsBit = 1u << 5;
constexpr std::uint8_t kWideReportId = 0x04;
constexpr int kWideReportBytes = 63;
constexpr int kReportsPerWideReport = 7;

// Extensions the relay uses. Firmware is negotiated only by the updater
// (Ctr2FirmwareUpdater), never on a relaying link.
constexpr std::uint32_t kHostCapabilities = capabilityBit(MessageType::AudioSpectrum)
    | capabilityBit(MessageType::RelayUdpPort) | capabilityBit(MessageType::AudioSpectrumLinear)
    | capabilityBit(MessageType::CwText) | kWideReportsBit;

// HID Feature report 0x02, exactly 8 bytes after the report ID (9 in all):
//   ['C']['X'][extension version 0x01][0x00][capabilities, 32 bits MSB first]
// Get returns the device's offer; Set tells it the subset the host will use.
namespace capabilities {
constexpr std::uint8_t kReportId = 0x02;
constexpr int kBytes = 8;
constexpr std::uint8_t kVersion = 0x01;
QByteArray encode(std::uint32_t caps);
// The offer in a Get reply (report ID stripped), or 0 for anything else:
// exactly kBytes, the magic, kVersion and a zero reserved byte.
std::uint32_t decode(const QByteArray& report);
}

// AudioSpectrum payload:
//   [bars N][low Hz hi][low Hz lo][span Hz hi][span Hz lo][N levels]
// Levels are 0..255, linear in dB from kFloorDb to 0 dBFS. With low > 0 the
// bars are log-spaced in frequency from low to span Hz (bar i covers
// bandEdgeHz(i)..bandEdgeHz(i + 1)); with low 0 they split 0..span evenly.
//
// AudioSpectrumLinear has the same payload, but its bars always split low..span
// evenly (linearEdgeHz), whatever low is: a window such as 200..1000 Hz around
// a CW pitch. A device that negotiates it gets it in place of AudioSpectrum
// where a window does not start at 0.
namespace spectrum {
constexpr int kMaxBars = 64;
constexpr int kFloorDb = -90;
constexpr int kHeaderBytes = 5;
QByteArray encode(int lowHz, int spanHz, const std::vector<float>& barsDb);
// Lower edge of bar i of n (i == n gives the top edge, spanHz).
double bandEdgeHz(int lowHz, int spanHz, int n, int i);
// Low edge of the log axis for a span: span / 60, kept within 20..100 Hz.
int displayLowHz(int spanHz);
// The span the bars cover: the passband width (filterHi - filterLo), up to a
// clean scale step (500 Hz .. 12 kHz), and never above the audio's Nyquist
// frequency. kDefaultSpanHz when there is no passband.
constexpr int kDefaultSpanHz = 4000;
int displaySpanHz(int filterLo, int filterHi, double sampleRate);
// Lower edge of bar i of n when low..span splits evenly (AudioSpectrumLinear).
double linearEdgeHz(int lowHz, int spanHz, int n, int i);
// CW: a linear window centered on the pitch, covering the passband (pitch +
// filterLo .. pitch + filterHi) with half its width to spare on each side, so
// a controller can show how far a signal is from zero beat. Kept within
// 0 Hz .. the audio's Nyquist frequency, still centered.
struct Window {
    int lowHz;
    int highHz;
};
Window cwWindow(int filterLo, int filterHi, int pitchHz, double sampleRate);
}

// Firmware: replaces a device's application over the link. The session opens
// in place of the host's Ready: after the device answers Hello, the host
// resets its counter and sends Begin. No radio connection opens. Payloads
// [op][...], integers big-endian:
//   host Begin    [0x01][image size u32][SHA-256 of the image]
//   host Data     [0x02][offset u32][1..kChunkBytes image bytes], in order
//   host End      [0x03]: verify, select the new image, restart into it
//   host Abort    [0x04]
//   host Switch   [0x05]: restart into the device's other firmware (the
//                 maker's own, kept beside this one); BadState if none
//   host Bootloader [0x06]: restart into the chip's ROM download mode, for
//                 an install there; BadMessage from firmware without it
//   device Status [0x81][op answered][Status][value u32]
// Every host message gets one Status; for Data, value is the bytes stored so
// far. The new image keeps itself only after it completes a link (answers
// Hello); a device that resets before then returns to its previous image.
namespace firmware {
constexpr int kChunkBytes = 512;
constexpr std::uint8_t kBegin = 0x01;
constexpr std::uint8_t kData = 0x02;
constexpr std::uint8_t kEnd = 0x03;
constexpr std::uint8_t kAbort = 0x04;
constexpr std::uint8_t kSwitch = 0x05;
constexpr std::uint8_t kBootloader = 0x06;
constexpr std::uint8_t kStatus = 0x81;
constexpr int kDataHeaderBytes = 5;
enum class Status : std::uint8_t {
    Ok = 0x00,
    BadState = 0x01,       // no session, or a second Begin
    BadOffset = 0x02,      // Data not at the next byte
    TooLarge = 0x03,       // image larger than the device's slot
    FlashError = 0x04,
    Rejected = 0x05,       // not a valid, correctly signed image
    HashMismatch = 0x06,   // stored bytes do not match Begin's hash
    BadMessage = 0x07,     // unknown op or wrong length
};
QByteArray encodeBegin(quint32 imageBytes, const QByteArray& sha256);
QByteArray encodeData(quint32 offset, const QByteArray& chunk);
QByteArray encodeEnd();
QByteArray encodeAbort();
QByteArray encodeSwitch();
QByteArray encodeBootloader();
struct Reply {
    std::uint8_t op{0};
    Status status{Status::Ok};
    quint32 value{0};
};
// A device Status payload; false for anything else.
bool decodeStatus(const QByteArray& payload, Reply* reply);
QString statusText(Status status);
}

// HID Feature report 0x03, Get only, exactly 24 bytes after the report ID:
//   ['F']['W'][layout 0x01][major][minor][patch][flags][0x00]
//   [build ID, 16 bytes of printable ASCII, NUL-padded]
// The device's firmware version, readable without a link. Devices without it
// (any stock CTR2) fail the Get.
namespace deviceinfo {
constexpr std::uint8_t kReportId = 0x03;
constexpr int kBytes = 24;
constexpr std::uint8_t kLayout = 0x01;
constexpr int kBuildBytes = 16;
constexpr std::uint8_t kPending = 0x01;   // not yet confirmed by a link; dropped on reset
constexpr std::uint8_t kSigned = 0x02;    // Firmware accepts only signed images
struct Info {
    int major{0};
    int minor{0};
    int patch{0};
    std::uint8_t flags{0};
    QString build;
    QString version() const;   // "major.minor.patch"
    bool pending() const { return flags & kPending; }
};
QByteArray encode(const Info& info);
// The fields of a Get reply (report ID stripped); false for anything else.
bool decode(const QByteArray& report, Info* info);
}

// CwText payload: [confidence][1..kMaxChars text bytes]. Text decoded by the
// CW decoder from the active slice, as it arrives (about a character at a
// time, word gaps as spaces), printable ASCII only. Confidence 0 is the best
// (decoder cost < 0.15), then 1 (< 0.35), 2 (< 0.60), 3, as AetherSDR's CW
// panel colors it.
namespace cwtext {
constexpr int kMaxChars = 32;
int confidence(float cost);
// The payload for a chunk of decoded text, or empty when nothing printable is
// left: newlines become spaces, other characters outside 0x20-0x7E are
// dropped, and text beyond kMaxChars is cut.
QByteArray encode(const QString& text, float cost);
}

struct Message {
    MessageType type{MessageType::Data};
    QByteArray payload;     // Data bytes, or the datagram without its port
    std::uint16_t port{0};  // Datagram only: the radio's UDP port
};

constexpr int packetsFor(int payloadBytes)
{
    return 1 + (payloadBytes + kDataBytesPerReport - 1) / kDataBytesPerReport;
}

constexpr std::uint8_t nextCounter(std::uint8_t c)
{
    return static_cast<std::uint8_t>((c + 1) & kCounterMask);
}

// Emits messages as reports. Data longer than kMaxPayloadBytes becomes
// several messages; control messages carry no payload.
class FrameEncoder {
public:
    void encodeData(const QByteArray& payload, std::vector<Report>* out);
    void encodeControl(MessageType type, std::vector<Report>* out);
    // One datagram of 1..kMaxDatagramBytes; returns false (and emits
    // nothing) when it does not fit.
    bool encodeDatagram(std::uint16_t port, const QByteArray& datagram, std::vector<Report>* out);
    // One extension message of 1..extensionMaxLength(type) bytes; false
    // (nothing emitted) for an undefined type or a bad size. Send only
    // negotiated types.
    bool encodeExtension(MessageType type, const QByteArray& payload, std::vector<Report>* out);
    // Sending Hello or Ready starts the counter again at 0.
    void reset() { m_counter = 0; }
    std::uint8_t counter() const { return m_counter; }

private:
    void encodeMessage(MessageType type, const char* data, int size, std::vector<Report>* out);

    std::uint8_t m_counter{0};
};

// Rebuilds messages from reports. Report position separates headers from
// data. Control messages are accepted with any counter and set the expected
// counter for the Data that follows; Data before any control message, or
// out of sequence, is an error. Any error is sticky until reset(): no
// partial message is ever released, and the owner ends the link.
class FrameReassembler {
public:
    enum class Error {
        None,
        BadReportSize,
        BadMarker,
        BadVersion,
        BadType,
        BadCounter,
        BadLength,
        PacketCountMismatch,
        NotStarted,
        CounterMismatch,
        DataCounterMismatch,
    };

    bool feed(const Report& report, std::vector<Message>* out);
    // Raw buffer from a host API; anything but exactly 8 bytes fails.
    bool feed(const std::uint8_t* data, int size, std::vector<Message>* out);

    void reset();
    // Extension types accepted (capabilityBit mask); kept across reset().
    // Bits for types with no defined length are dropped.
    void setExtensions(std::uint32_t caps);
    bool failed() const { return m_error != Error::None; }
    Error error() const { return m_error; }
    QString errorText() const;
    bool isMidMessage() const { return m_packetsLeft > 0; }
    int bufferedBytes() const { return static_cast<int>(m_partial.size()); }

private:
    bool fail(Error error);
    bool feedHeader(const Report& r, std::vector<Message>* out);
    bool feedData(const Report& r, std::vector<Message>* out);

    Error m_error{Error::None};
    std::uint32_t m_extensions{0};
    bool m_started{false};
    std::uint8_t m_expected{0};
    std::uint8_t m_messageCounter{0};
    MessageType m_messageType{MessageType::Data};
    int m_packetsLeft{0};
    int m_bytesLeft{0};
    QByteArray m_partial;
};

} // namespace AetherSDR::ctr2hid
