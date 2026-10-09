#include "Ctr2HidFraming.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace AetherSDR::ctr2hid {

void FrameEncoder::encodeData(const QByteArray& payload, std::vector<Report>* out)
{
    int offset = 0;
    while (offset < payload.size()) {
        const int size = std::min<int>(kMaxPayloadBytes, static_cast<int>(payload.size()) - offset);
        encodeMessage(MessageType::Data, payload.constData() + offset, size, out);
        offset += size;
    }
}

void FrameEncoder::encodeControl(MessageType type, std::vector<Report>* out)
{
    encodeMessage(type, nullptr, 0, out);
}

bool FrameEncoder::encodeDatagram(std::uint16_t port, const QByteArray& datagram,
                                  std::vector<Report>* out)
{
    if (datagram.isEmpty() || datagram.size() > kMaxDatagramBytes) {
        return false;
    }
    QByteArray payload;
    payload.reserve(2 + datagram.size());
    payload.append(static_cast<char>(port >> 8));
    payload.append(static_cast<char>(port & 0xFF));
    payload.append(datagram);
    encodeMessage(MessageType::Datagram, payload.constData(), static_cast<int>(payload.size()), out);
    return true;
}

bool FrameEncoder::encodeExtension(MessageType type, const QByteArray& payload,
                                   std::vector<Report>* out)
{
    const auto t = static_cast<std::uint8_t>(type);
    if (t < kExtensionFirst || t > kExtensionLast || payload.isEmpty()
        || payload.size() > extensionMaxLength(type)) {
        return false;
    }
    encodeMessage(type, payload.constData(), static_cast<int>(payload.size()), out);
    return true;
}

int extensionMaxLength(MessageType type)
{
    switch (type) {
    case MessageType::AudioSpectrum:
        return spectrum::kHeaderBytes + spectrum::kMaxBars;
    case MessageType::RelayUdpPort:
        return 2;
    default:
        return 0;
    }
}

void FrameReassembler::setExtensions(std::uint32_t caps)
{
    std::uint32_t defined = 0;
    for (int t = kExtensionFirst; t <= kExtensionLast; ++t) {
        if (extensionMaxLength(static_cast<MessageType>(t)) > 0) {
            defined |= 1u << (t - kExtensionFirst);
        }
    }
    m_extensions = caps & defined;
}

QByteArray capabilities::encode(std::uint32_t caps)
{
    QByteArray r(kBytes, '\0');
    r[0] = 'C';
    r[1] = 'X';
    r[2] = static_cast<char>(kVersion);
    for (int i = 0; i < 4; ++i) {
        r[4 + i] = static_cast<char>((caps >> (24 - 8 * i)) & 0xFF);
    }
    return r;
}

std::uint32_t capabilities::decode(const QByteArray& r)
{
    if (r.size() < kBytes || r[0] != 'C' || r[1] != 'X'
        || static_cast<std::uint8_t>(r[2]) != kVersion) {
        return 0;
    }
    std::uint32_t caps = 0;
    for (int i = 0; i < 4; ++i) {
        caps = (caps << 8) | static_cast<std::uint8_t>(r[4 + i]);
    }
    return caps;
}

QByteArray spectrum::encode(int lowHz, int spanHz, const std::vector<float>& barsDb)
{
    const int n = std::min<int>(kMaxBars, static_cast<int>(barsDb.size()));
    QByteArray p;
    p.reserve(kHeaderBytes + n);
    p.append(static_cast<char>(n));
    p.append(static_cast<char>((lowHz >> 8) & 0xFF));
    p.append(static_cast<char>(lowHz & 0xFF));
    p.append(static_cast<char>((spanHz >> 8) & 0xFF));
    p.append(static_cast<char>(spanHz & 0xFF));
    for (int i = 0; i < n; ++i) {
        const float db = std::isfinite(barsDb[i]) ? barsDb[i] : float(kFloorDb);
        const float scaled = (db - kFloorDb) * 255.0f / -kFloorDb;
        p.append(static_cast<char>(std::clamp<int>(std::lround(scaled), 0, 255)));
    }
    return p;
}

double spectrum::bandEdgeHz(int lowHz, int spanHz, int n, int i)
{
    if (n <= 0 || i <= 0) {
        return lowHz > 0 ? lowHz : 0.0;
    }
    if (i >= n) {
        return spanHz;
    }
    if (lowHz <= 0 || lowHz >= spanHz) {
        return double(spanHz) * i / n;   // linear
    }
    return lowHz * std::pow(double(spanHz) / lowHz, double(i) / n);
}

int spectrum::displayLowHz(int spanHz)
{
    return std::clamp(spanHz / 60, 20, 100);
}

int spectrum::displaySpanHz(int filterLo, int filterHi, double sampleRate)
{
    static constexpr int kSteps[] = {500, 600, 800, 1000, 1200, 1500, 2000, 2500, 3000,
                                     4000, 5000, 6000, 8000, 10000, 12000, 16000, 20000};
    const int width = filterHi - filterLo;
    const int nyquist = sampleRate > 0 ? static_cast<int>(sampleRate / 2) : 12000;
    if (width <= 0) {
        return std::min(kDefaultSpanHz, nyquist);
    }
    int span = kSteps[std::size(kSteps) - 1];
    for (int step : kSteps) {
        if (step >= width) {
            span = step;
            break;
        }
    }
    return std::min(span, nyquist);
}

void FrameEncoder::encodeMessage(MessageType type, const char* data, int size,
                                 std::vector<Report>* out)
{
    const int packets = packetsFor(size);
    out->push_back(Report{kMarker, kVersion, m_counter, static_cast<std::uint8_t>(type),
                          static_cast<std::uint8_t>(packets >> 8),
                          static_cast<std::uint8_t>(packets & 0xFF),
                          static_cast<std::uint8_t>(size >> 8),
                          static_cast<std::uint8_t>(size & 0xFF)});
    for (int p = 1; p < packets; ++p) {
        Report r{};
        r[0] = m_counter;
        const int start = (p - 1) * kDataBytesPerReport;
        const int n = std::min(kDataBytesPerReport, size - start);
        for (int i = 0; i < n; ++i) {
            r[1 + i] = static_cast<std::uint8_t>(data[start + i]);
        }
        out->push_back(r);
    }
    m_counter = nextCounter(m_counter);
}

void FrameReassembler::reset()
{
    m_error = Error::None;
    m_started = false;
    m_expected = 0;
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

bool FrameReassembler::feed(const std::uint8_t* data, int size, std::vector<Message>* out)
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

bool FrameReassembler::feed(const Report& report, std::vector<Message>* out)
{
    if (failed()) {
        return false;
    }
    return m_packetsLeft > 0 ? feedData(report, out) : feedHeader(report, out);
}

bool FrameReassembler::feedHeader(const Report& r, std::vector<Message>* out)
{
    if (r[0] != kMarker) {
        return fail(Error::BadMarker);
    }
    if (r[1] != kVersion) {
        return fail(Error::BadVersion);
    }
    if (r[2] > kCounterMask) {
        return fail(Error::BadCounter);
    }
    const bool extension = r[3] >= kExtensionFirst && r[3] <= kExtensionLast;
    if (r[3] > static_cast<std::uint8_t>(MessageType::Datagram)
        && !(extension && (m_extensions & (1u << (r[3] - kExtensionFirst))))) {
        return fail(Error::BadType);
    }
    const auto type = static_cast<MessageType>(r[3]);
    const int packets = (r[4] << 8) | r[5];
    const int length = (r[6] << 8) | r[7];
    const bool control = type == MessageType::Hello || type == MessageType::Ready
        || type == MessageType::Closed;
    const bool lengthOk = control ? length == 0
        : extension ? (length >= 1 && length <= extensionMaxLength(type))
        : type == MessageType::Data ? (length >= 1 && length <= kMaxPayloadBytes)
                                    : (length >= 3 && length <= kMaxMessageBytes);
    if (!lengthOk) {
        return fail(Error::BadLength);
    }
    if (packets != packetsFor(length)) {
        return fail(Error::PacketCountMismatch);
    }
    if (control) {
        m_started = true;
        m_expected = nextCounter(r[2]);
        out->push_back(Message{type, {}, 0});
        return true;
    }
    if (!m_started) {
        return fail(Error::NotStarted);
    }
    if (r[2] != m_expected) {
        return fail(Error::CounterMismatch);
    }
    m_messageType = type;
    m_messageCounter = r[2];
    m_packetsLeft = packets - 1;
    m_bytesLeft = length;
    m_partial.clear();
    m_partial.reserve(length);
    return true;
}

bool FrameReassembler::feedData(const Report& r, std::vector<Message>* out)
{
    if (r[0] != m_messageCounter) {
        return fail(Error::DataCounterMismatch);
    }
    const int n = std::min(kDataBytesPerReport, m_bytesLeft);
    m_partial.append(reinterpret_cast<const char*>(r.data() + 1), n);
    m_bytesLeft -= n;
    if (--m_packetsLeft == 0) {
        if (m_messageType == MessageType::Datagram) {
            const auto hi = static_cast<std::uint8_t>(m_partial[0]);
            const auto lo = static_cast<std::uint8_t>(m_partial[1]);
            out->push_back(Message{MessageType::Datagram, m_partial.mid(2),
                                   static_cast<std::uint16_t>((hi << 8) | lo)});
        } else {
            out->push_back(Message{m_messageType, m_partial, 0});
        }
        m_partial.clear();
        m_expected = nextCounter(m_messageCounter);
    }
    return true;
}

QString FrameReassembler::errorText() const
{
    switch (m_error) {
    case Error::None:                return {};
    case Error::BadReportSize:       return QStringLiteral("HID report is not 8 bytes");
    case Error::BadMarker:           return QStringLiteral("Expected a message header (0xFF)");
    case Error::BadVersion:          return QStringLiteral("Unsupported link version");
    case Error::BadType:             return QStringLiteral("Unknown or unnegotiated message type");
    case Error::BadCounter:          return QStringLiteral("Message counter above 0x7F");
    case Error::BadLength:           return QStringLiteral("Payload length invalid for this message type");
    case Error::PacketCountMismatch: return QStringLiteral("Packet count does not match payload length");
    case Error::NotStarted:          return QStringLiteral("Data before the link was started");
    case Error::CounterMismatch:     return QStringLiteral("Message counter out of sequence");
    case Error::DataCounterMismatch: return QStringLiteral("Data report counter does not match its header");
    }
    return {};
}

} // namespace AetherSDR::ctr2hid
