#pragma once

#include <QByteArray>
#include <QString>

#include <array>
#include <cstdint>
#include <vector>

namespace AetherSDR::ctr2hid {

// DRAFT wire format for carrying an opaque radio TCP byte stream over the
// CTR2's 8-byte HID reports. Nothing here is agreed with the firmware yet;
// every negotiable value lives in WireFormat so the agreement is one edit.
// A Report is the 8 payload bytes only, excluding any HID report ID.
// Design: docs/ctr2-usb-relay-design.md ("Host-side status").
//
// Header report: [marker][version][counter][packets lo][packets hi]
//                [length lo][length hi][reserved = 0]
// Data report:   [counter][up to 7 payload bytes][padding]
constexpr int kReportBytes = 8;
constexpr int kDataBytesPerReport = 7;
using Report = std::array<std::uint8_t, kReportBytes>;

struct WireFormat {
    std::uint8_t marker{0xA5};
    std::uint8_t version{1};
    // Counters start here on a fresh connection and wrap modulo 256,
    // independently per direction.
    std::uint8_t firstCounter{0};
    // Upper bound on one message's payload; the length field allows 65535.
    int maxMessageBytes{4096};
};

constexpr int packetsFor(int payloadBytes)
{
    return (payloadBytes + kDataBytesPerReport - 1) / kDataBytesPerReport;
}

// Splits a byte stream into messages of at most maxMessageBytes and emits
// each as one header report followed by its data reports, in order.
class FrameEncoder {
public:
    explicit FrameEncoder(const WireFormat& format = {});

    void encode(const QByteArray& payload, std::vector<Report>* out);
    void reset();
    std::uint8_t nextCounter() const { return m_counter; }

private:
    void encodeMessage(const char* data, int size, std::vector<Report>* out);

    WireFormat m_format;
    std::uint8_t m_counter;
};

// Rebuilds the byte stream from reports. Report position, not content,
// distinguishes headers from data: while a message is incomplete every report
// is data, even one whose first byte equals the marker. Any violation puts
// the reassembler into a failed state that refuses all input until reset();
// the owner must end the connection, never release a partial message.
class FrameReassembler {
public:
    enum class Error {
        None,
        BadReportSize,
        BadMarker,
        BadVersion,
        BadReserved,
        CounterMismatch,
        BadLength,
        PacketCountMismatch,
        DataCounterMismatch,
    };

    explicit FrameReassembler(const WireFormat& format = {});

    // Appends a completed message's payload to *out. Returns false on error.
    bool feed(const Report& report, QByteArray* out);
    // Host APIs hand over raw buffers; anything but exactly 8 bytes fails.
    bool feed(const std::uint8_t* data, int size, QByteArray* out);

    void reset();
    bool failed() const { return m_error != Error::None; }
    Error error() const { return m_error; }
    QString errorText() const;
    bool isMidMessage() const { return m_packetsLeft > 0; }
    int bufferedBytes() const { return static_cast<int>(m_partial.size()); }
    std::uint8_t expectedCounter() const { return m_expectedCounter; }

private:
    bool fail(Error error);
    bool feedHeader(const Report& report);
    bool feedData(const Report& report, QByteArray* out);

    WireFormat m_format;
    Error m_error{Error::None};
    std::uint8_t m_expectedCounter;
    std::uint8_t m_messageCounter{0};
    int m_packetsLeft{0};
    int m_bytesLeft{0};
    QByteArray m_partial;
};

} // namespace AetherSDR::ctr2hid
