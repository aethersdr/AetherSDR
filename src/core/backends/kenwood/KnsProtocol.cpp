#include "KnsProtocol.h"

#include <QtEndian>
#include <QStringList>
#include <QStringDecoder>

#include <algorithm>
#include <cmath>
#include <limits>

namespace AetherSDR::kenwood {
QStringList transmitAudioInputChoices()
{
    return {QStringLiteral("Microphone"), QStringLiteral("ACC 2"), QStringLiteral("USB Audio"),
        QStringLiteral("LAN"), QStringLiteral("Microphone + ACC 2"),
        QStringLiteral("Microphone + USB Audio"), QStringLiteral("Microphone + LAN")};
}

std::optional<TransmitAudioInput> decodeTransmitAudioInput(const QByteArray& message)
{
    if (message.size() != 5 || !message.startsWith("MS") || message[2] < '0' || message[2] > '1'
        || message[3] < '0' || message[3] > '1' || message[4] < '0' || message[4] > '3'
        || (message[3] == '0' && message[4] == '0')) { return std::nullopt; }
    const int rear = message[4] - '0';
    const int selection = message[3] == '0' ? rear : rear == 0 ? 0 : rear + 3;
    return TransmitAudioInput{message[2] - '0', selection};
}

std::optional<QByteArray> encodeTransmitAudioInput(const TransmitAudioInput& input)
{
    if (input.path < 0 || input.path > 1 || input.selection < 0 || input.selection > 6) { return std::nullopt; }
    const bool microphone = input.selection == 0 || input.selection >= 4;
    const int rear = input.selection >= 4 ? input.selection - 3 : input.selection;
    return "MS" + QByteArray::number(input.path) + (microphone ? "1" : "0") + QByteArray::number(rear);
}

std::optional<qint64> maximumFrequencyForVariant(const QString& variant)
{
    if (variant.isEmpty() || variant == QLatin1String("k")) { return kDefaultMaximumFrequencyHz; }
    if (variant == QLatin1String("e")) { return kEuropeanMaximumFrequencyHz; }
    return std::nullopt;
}
namespace {
bool printable(const QByteArray& bytes)
{
    return std::all_of(bytes.cbegin(), bytes.cend(), [](char value) {
        const unsigned char byte = static_cast<unsigned char>(value);
        return byte >= 32 && byte <= 126 && byte != ';';
    });
}
int hexDigit(char value)
{
    if (value >= '0' && value <= '9') { return value - '0'; }
    if (value >= 'A' && value <= 'F') { return value - 'A' + 10; }
    if (value >= 'a' && value <= 'f') { return value - 'a' + 10; }
    return -1;
}
constexpr char kModeCodes[] = "12345679ABCDEF";
const QStringList kModes = {"LSB", "USB", "CW", "FM", "AM", "RTTY", "CWR",
                            "RTTYR", "PSK", "PSKR", "DIGL", "DIGU", "DFM", "DAM"};
}

void KnsProtocol::reset()
{
    m_state = State::Closed;
    m_tail.clear();
    m_login.fill('\0');
    m_login.clear();
    m_deadlineMs = m_keepaliveMs = m_lastReceiveMs = 0;
}

void KnsProtocol::fail(Events& events, const QString& error)
{
    reset();
    m_state = State::Failed;
    events.error = error;
    events.messages.clear();
    events.outgoing.clear();
    events.authenticated = false;
}

bool KnsProtocol::credentialsValid(const QString& username, const QString& password)
{
    if (username.isEmpty() || username.size() > 32 || password.isEmpty() || password.size() > 32) { return false; }
    const QByteArray user = username.toLatin1();
    const QByteArray pass = password.toLatin1();
    return QString::fromLatin1(user) == username && QString::fromLatin1(pass) == password
        && printable(user) && printable(pass);
}

KnsProtocol::Events KnsProtocol::start(const QString& username, const QString& password,
                                      bool administrator, qint64 nowMs)
{
    reset();
    Events events;
    if (!credentialsValid(username, password) || nowMs < 0) {
        fail(events, QStringLiteral("KNS credentials must contain 1–32 ASCII characters without semicolons."));
        return events;
    }
    const QByteArray user = username.toLatin1();
    const QByteArray pass = password.toLatin1();
    m_login = "##ID" + QByteArray(administrator ? "0" : "1")
        + QByteArray::number(user.size()).rightJustified(2, '0')
        + QByteArray::number(pass.size()).rightJustified(2, '0') + user + pass + ';';
    m_state = State::Greeting;
    m_deadlineMs = nowMs + 5000;
    m_lastReceiveMs = nowMs;
    events.outgoing.append("##CN;");
    return events;
}

std::optional<QByteArray> KnsProtocol::command(const QByteArray& body)
{
    if (body.isEmpty() || body.size() > kMaximumMessageBytes || !printable(body)) {
        return std::nullopt;
    }
    return body + ';';
}

KnsProtocol::Events KnsProtocol::receive(const QByteArray& bytes, qint64 nowMs)
{
    Events events;
    if (m_state == State::Closed || m_state == State::Failed) { return events; }
    // Process incrementally: even a huge input cannot enlarge the tail beyond
    // one bounded message. A socket owner also caps reads per event-loop turn.
    for (char byte : bytes) {
        if (byte != ';') {
            if (static_cast<unsigned char>(byte) < 32 || byte == char(0x7f)
                || m_tail.size() >= kMaximumMessageBytes) {
                fail(events, QStringLiteral("Malformed or oversized KNS message."));
                return events;
            }
            m_tail.append(byte);
            continue;
        }
        const QByteArray message = m_tail;
        m_tail.clear();
        if (message.isEmpty()) { continue; }
        // LAN text uses UTF-8 (PC Command Reference p.1). Validate only once
        // a frame is complete: a multibyte character can span socket reads.
        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString decoded = decoder.decode(message);
        if (decoder.hasError() || decoded.toUtf8() != message) {
            fail(events, QStringLiteral("Malformed KNS text encoding."));
            return events;
        }
        if (m_state == State::Greeting) {
            if (message == "##CN0") {
                fail(events, QStringLiteral("The radio refused the KNS connection or is busy."));
                return events;
            }
            if (message != "##CN1") {
                fail(events, QStringLiteral("Unexpected KNS connection response."));
                return events;
            }
            m_state = State::Authenticating;
            events.outgoing.append(m_login);
            m_login.fill('\0');
            m_login.clear();
        } else if (m_state == State::Authenticating) {
            if (message != "##ID1") {
                fail(events, QStringLiteral("KNS authentication failed."));
                return events;
            }
            m_state = State::Ready;
            m_lastReceiveMs = nowMs;
            m_keepaliveMs = nowMs + 5000;
            events.authenticated = true;
        } else {
            // Only complete messages count as liveness, not partial garbage.
            m_lastReceiveMs = nowMs;
            events.messages.append(message);
        }
    }
    return events;
}

KnsProtocol::Events KnsProtocol::tick(qint64 nowMs)
{
    Events events;
    if ((m_state == State::Greeting || m_state == State::Authenticating)
        && nowMs >= m_deadlineMs) {
        fail(events, QStringLiteral("KNS login timed out."));
    } else if (m_state == State::Ready) {
        if (nowMs - m_lastReceiveMs >= 15000) {
            fail(events, QStringLiteral("KNS radio response timed out."));
        } else if (nowMs >= m_keepaliveMs) {
            events.outgoing.append("PS;");
            m_keepaliveMs = nowMs + 5000;
        }
    }
    return events;
}

std::optional<ScopeLine> decodeScope(const QByteArray& message)
{
    if (!message.startsWith("##DD2") || message.size() != 1285) { return std::nullopt; }
    ScopeLine line;
    line.levels.reserve(640);
    for (qsizetype index = 5; index < message.size(); index += 2) {
        const int high = hexDigit(message[index]);
        const int low = hexDigit(message[index + 1]);
        if (high < 0 || low < 0) { return std::nullopt; }
        const int level = high * 16 + low;
        if (level > 140) { return std::nullopt; }
        // Kenwood PC Command Reference (2019-01-30), p.70: 00h = 0 dB,
        // 8Ch = -100 dB. These are display-relative, never antenna dBm.
        line.levels.append(-100.0f * static_cast<float>(level) / 140.0f);
    }
    return line;
}

std::optional<qint64> decodeFrequency(const QByteArray& message)
{
    if (message.size() != 13 || (message.first(2) != "FA" && message.first(2) != "FB")) {
        return std::nullopt;
    }
    qint64 hz = 0;
    for (qsizetype index = 2; index < message.size(); ++index) {
        const char value = message[index];
        if (value < '0' || value > '9') { return std::nullopt; }
        hz = hz * 10 + value - '0';
    }
    if (hz < kMinimumFrequencyHz || hz > kEuropeanMaximumFrequencyHz) { return std::nullopt; }
    return hz;
}

std::optional<QByteArray> encodeFrequency(char vfo, qint64 hz)
{
    if ((vfo != 'A' && vfo != 'B') || hz < kMinimumFrequencyHz || hz > kEuropeanMaximumFrequencyHz) { return std::nullopt; }
    return QByteArray("F") + vfo + QByteArray::number(hz).rightJustified(11, '0');
}

std::optional<QString> decodeMode(char code)
{
    for (qsizetype index = 0; index < kModes.size(); ++index) {
        if (kModeCodes[index] == code) { return kModes[index]; }
    }
    return std::nullopt;
}

std::optional<char> encodeMode(const QString& mode)
{
    const qsizetype index = kModes.indexOf(mode);
    if (index < 0) { return std::nullopt; }
    return kModeCodes[index];
}

std::optional<VfoState> decodeVfoState(const QByteArray& message)
{
    // PC Command Reference (2019-01-30), p.59: physical VFO identity,
    // unlike OM's left/right display positions.
    if (message.size() != 15 || !message.startsWith("SF") || (message[2] != '0' && message[2] != '1')) { return std::nullopt; }
    const std::optional<qint64> frequency = decodeFrequency(QByteArray("FA") + message.sliced(3, 11));
    const std::optional<QString> mode = decodeMode(message[14]);
    if (!frequency || !mode) { return std::nullopt; }
    return VfoState{message[2] - '0', *frequency, *mode};
}

std::optional<bool> decodeStatusTransmit(const QByteArray& message)
{
    // TS-890S firmware 1.01's compatibility IF status matches Hamlib's
    // kenwood_get_ptt layout: 37 bytes, TX/RX at zero-based offset 28.
    if (message.size() != 37 || !message.startsWith("IF")
        || !decodeFrequency(QByteArray("FA") + message.sliced(2, 11))
        || !decodeMode(message[29]) || (message[28] != '0' && message[28] != '1')) { return std::nullopt; }
    for (qsizetype index = 13; index < message.size(); ++index) {
        const char value = message[index];
        if (index == 29 || (index < 19 && value == ' ')
            || (index == 19 && (value == '+' || value == '-'))) { continue; }
        if (value < '0' || value > '9') { return std::nullopt; }
    }
    return message[28] == '1';
}

std::optional<AudioPacket> decodeAudio(const QByteArray& packet)
{
    if (packet.size() < 12 || packet.size() > 2048) { return std::nullopt; }
    const auto* data = reinterpret_cast<const uchar*>(packet.constData());
    if ((data[0] >> 6) != 2 || (data[1] & 0x7f) != 96) { return std::nullopt; }
    qsizetype offset = 12 + 4 * (data[0] & 15);
    if (offset > packet.size()) { return std::nullopt; }
    if ((data[0] & 0x10) != 0) {
        if (offset + 4 > packet.size()) { return std::nullopt; }
        offset += 4 + 4 * qFromBigEndian<quint16>(data + offset + 2);
        if (offset > packet.size()) { return std::nullopt; }
    }
    qsizetype end = packet.size();
    if ((data[0] & 0x20) != 0) {
        const int padding = data[end - 1];
        if (padding == 0 || padding > end - offset) { return std::nullopt; }
        end -= padding;
    }
    if (end <= offset || (end - offset) % 2 != 0 || end - offset > 1280) { return std::nullopt; }
    AudioPacket result;
    result.sequence = qFromBigEndian<quint16>(data + 2);
    result.timestamp = qFromBigEndian<quint32>(data + 4);
    result.source = qFromBigEndian<quint32>(data + 8);
    for (qsizetype index = offset; index < end; index += 2) {
        result.samples.append(static_cast<float>(qFromLittleEndian<qint16>(data + index)) / 32768.0f);
    }
    return result;
}

std::optional<QByteArray> encodeTxAudio(const QVector<float>& samples, quint16 sequence, float gain)
{
    if (samples.size() != 320 || !std::isfinite(gain) || gain <= 0 || gain > 1) {
        return std::nullopt;
    }
    QByteArray packet(652, '\0');
    auto* data = reinterpret_cast<uchar*>(packet.data());
    data[0] = 0x80;
    data[1] = 96;
    qToBigEndian(sequence, data + 2);
    qToBigEndian<quint32>(0x38393000, data + 8);
    for (qsizetype index = 0; index < samples.size(); ++index) {
        if (!std::isfinite(samples[index])) { return std::nullopt; }
        const float sample = std::clamp(samples[index], -1.0f, 1.0f);
        const int value = static_cast<int>(std::lround(sample * gain * 32767.0f)) + 32768;
        qToBigEndian(static_cast<quint16>(value), data + 12 + index * 2);
    }
    return packet;
}

void AudioSequence::reset()
{
    m_seen = false;
    m_restartPackets = 0;
}

AudioSequence::Result AudioSequence::accept(quint16 sequence, quint32 source, qint64 nowMs)
{
    constexpr int kRestartPackets = 10;
    Result result = Result::First;
    if (m_seen && source == m_source && nowMs - m_lastMs < 1000) {
        const quint16 distance = static_cast<quint16>(sequence - m_next);
        if (distance >= 32768) {
            // A restarted sender can retain its SSRC and reset its sequence.
            // Require a sustained new numbering run; duplicate or disordered
            // packets cannot advance the restart proof or rewind live traffic.
            m_restartPackets = m_restartPackets > 0 && sequence == m_restartNext
                ? m_restartPackets + 1 : 1;
            m_restartNext = static_cast<quint16>(sequence + 1);
            if (m_restartPackets < kRestartPackets) { return Result::Drop; }
            result = Result::Restart;
        } else {
            result = distance == 0 ? Result::Continuous : Result::Gap;
        }
    } else if (m_seen) {
        result = Result::Restart;
    }
    m_restartPackets = 0;
    m_seen = true;
    m_source = source;
    m_next = static_cast<quint16>(sequence + 1);
    m_lastMs = nowMs;
    return result;
}
} // namespace AetherSDR::kenwood
