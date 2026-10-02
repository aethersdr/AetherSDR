#include "Ctr2HidFraming.h"

#include <algorithm>

namespace AetherSDR::ctr2hid {

namespace {

constexpr int kMaxLengthField = 0xFFFF;

int effectiveMax(const WireFormat& format)
{
    return std::clamp(format.maxMessageBytes, 1, kMaxLengthField);
}

} // namespace

FrameEncoder::FrameEncoder(const WireFormat& format)
    : m_format(format)
    , m_counter(format.firstCounter)
{
}

void FrameEncoder::reset()
{
    m_counter = m_format.firstCounter;
}

void FrameEncoder::encode(const QByteArray& payload, std::vector<Report>* out)
{
    const int maxBytes = effectiveMax(m_format);
    int offset = 0;
    while (offset < payload.size()) {
        const int size = std::min<int>(maxBytes, static_cast<int>(payload.size()) - offset);
        encodeMessage(payload.constData() + offset, size, out);
        offset += size;
    }
}

void FrameEncoder::encodeMessage(const char* data, int size, std::vector<Report>* out)
{
    const int packets = packetsFor(size);
    Report header{};
    header[0] = m_format.marker;
    header[1] = m_format.version;
    header[2] = m_counter;
    header[3] = static_cast<std::uint8_t>(packets & 0xFF);
    header[4] = static_cast<std::uint8_t>((packets >> 8) & 0xFF);
    header[5] = static_cast<std::uint8_t>(size & 0xFF);
    header[6] = static_cast<std::uint8_t>((size >> 8) & 0xFF);
    header[7] = 0;
    out->push_back(header);

    for (int p = 0; p < packets; ++p) {
        Report r{};
        r[0] = m_counter;
        const int start = p * kDataBytesPerReport;
        const int n = std::min(kDataBytesPerReport, size - start);
        for (int i = 0; i < n; ++i) {
            r[1 + i] = static_cast<std::uint8_t>(data[start + i]);
        }
        out->push_back(r);
    }
    m_counter = static_cast<std::uint8_t>(m_counter + 1);
}

FrameReassembler::FrameReassembler(const WireFormat& format)
    : m_format(format)
    , m_expectedCounter(format.firstCounter)
{
}

void FrameReassembler::reset()
{
    m_error = Error::None;
    m_expectedCounter = m_format.firstCounter;
    m_messageCounter = 0;
    m_packetsLeft = 0;
    m_bytesLeft = 0;
    m_partial.clear();
}

bool FrameReassembler::fail(Error error)
{
    m_error = error;
    m_packetsLeft = 0;
    m_bytesLeft = 0;
    m_partial.clear();
    return false;
}

bool FrameReassembler::feed(const std::uint8_t* data, int size, QByteArray* out)
{
    if (failed()) {
        return false;
    }
    if (!data || size != kReportBytes) {
        return fail(Error::BadReportSize);
    }
    Report r{};
    std::copy(data, data + kReportBytes, r.begin());
    return feed(r, out);
}

bool FrameReassembler::feed(const Report& report, QByteArray* out)
{
    if (failed()) {
        return false;
    }
    if (m_packetsLeft > 0) {
        return feedData(report, out);
    }
    return feedHeader(report);
}

bool FrameReassembler::feedHeader(const Report& r)
{
    if (r[0] != m_format.marker) {
        return fail(Error::BadMarker);
    }
    if (r[1] != m_format.version) {
        return fail(Error::BadVersion);
    }
    if (r[7] != 0) {
        return fail(Error::BadReserved);
    }
    if (r[2] != m_expectedCounter) {
        return fail(Error::CounterMismatch);
    }
    const int packets = r[3] | (r[4] << 8);
    const int length = r[5] | (r[6] << 8);
    if (length < 1 || length > effectiveMax(m_format)) {
        return fail(Error::BadLength);
    }
    if (packets != packetsFor(length)) {
        return fail(Error::PacketCountMismatch);
    }
    m_messageCounter = r[2];
    m_packetsLeft = packets;
    m_bytesLeft = length;
    m_partial.clear();
    m_partial.reserve(length);
    return true;
}

bool FrameReassembler::feedData(const Report& r, QByteArray* out)
{
    if (r[0] != m_messageCounter) {
        return fail(Error::DataCounterMismatch);
    }
    const int n = std::min(kDataBytesPerReport, m_bytesLeft);
    m_partial.append(reinterpret_cast<const char*>(r.data() + 1), n);
    m_bytesLeft -= n;
    --m_packetsLeft;
    if (m_packetsLeft == 0) {
        out->append(m_partial);
        m_partial.clear();
        m_expectedCounter = static_cast<std::uint8_t>(m_messageCounter + 1);
    }
    return true;
}

QString FrameReassembler::errorText() const
{
    switch (m_error) {
    case Error::None:                return {};
    case Error::BadReportSize:       return QStringLiteral("HID report is not 8 bytes");
    case Error::BadMarker:           return QStringLiteral("Expected a message header; marker byte wrong");
    case Error::BadVersion:          return QStringLiteral("Unsupported relay protocol version");
    case Error::BadReserved:         return QStringLiteral("Header reserved byte is not zero");
    case Error::CounterMismatch:     return QStringLiteral("Message counter out of sequence");
    case Error::BadLength:           return QStringLiteral("Message length is zero or exceeds the maximum");
    case Error::PacketCountMismatch: return QStringLiteral("Packet count does not match message length");
    case Error::DataCounterMismatch: return QStringLiteral("Data report counter does not match its header");
    }
    return {};
}

} // namespace AetherSDR::ctr2hid
