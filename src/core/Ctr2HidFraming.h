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
};

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
    bool m_started{false};
    std::uint8_t m_expected{0};
    std::uint8_t m_messageCounter{0};
    MessageType m_messageType{MessageType::Data};
    int m_packetsLeft{0};
    int m_bytesLeft{0};
    QByteArray m_partial;
};

} // namespace AetherSDR::ctr2hid
