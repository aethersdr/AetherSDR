#include "core/tnc/KissFraming.h"

namespace AetherSDR::kiss {

QByteArray escape(const QByteArray& in)
{
    QByteArray out;
    out.reserve(in.size() + 8);
    for (const char ch : in) {
        const quint8 b = static_cast<quint8>(ch);
        if (b == kFend) {
            out.append(static_cast<char>(kFesc));
            out.append(static_cast<char>(kTfend));
        } else if (b == kFesc) {
            out.append(static_cast<char>(kFesc));
            out.append(static_cast<char>(kTfesc));
        } else {
            out.append(ch);
        }
    }
    return out;
}

QByteArray unescape(const QByteArray& in)
{
    QByteArray out;
    out.reserve(in.size());
    bool escape = false;
    for (const char ch : in) {
        const quint8 b = static_cast<quint8>(ch);
        if (escape) {
            if (b == kTfend) {
                out.append(static_cast<char>(kFend));
            } else if (b == kTfesc) {
                out.append(static_cast<char>(kFesc));
            } else {
                out.append(ch); // invalid escape — keep the byte literally
            }
            escape = false;
        } else if (b == kFesc) {
            escape = true;
        } else {
            out.append(ch);
        }
    }
    return out;
}

QByteArray encodeDataFrame(const QByteArray& ax25NoFcs, quint8 port)
{
    QByteArray out;
    out.reserve(ax25NoFcs.size() + 4);
    out.append(static_cast<char>(kFend));
    out.append(static_cast<char>((port << 4) | kCmdData));
    out.append(escape(ax25NoFcs));
    out.append(static_cast<char>(kFend));
    return out;
}

QVector<QByteArray> Decoder::feed(const QByteArray& bytes)
{
    QVector<QByteArray> frames;
    for (const char ch : bytes) {
        const quint8 b = static_cast<quint8>(ch);

        if (b == kFend) {
            if (m_synced && !m_overflow && !m_frame.isEmpty())
                frames.append(unescape(m_frame));
            m_frame.clear();
            m_synced = true;
            m_overflow = false;
            continue;
        }

        if (!m_synced)
            continue; // ignore bytes before the first frame boundary

        if (m_overflow)
            continue; // drop the rest of an oversized frame until next FEND

        m_frame.append(ch);
        if (m_frame.size() > kMaxFrameBytes) {
            m_frame.clear();
            m_overflow = true;
        }
    }
    return frames;
}

void Decoder::reset()
{
    m_frame.clear();
    m_synced = false;
    m_overflow = false;
}

bool splitTypeByte(const QByteArray& frameWithType, quint8& port, quint8& command,
                   QByteArray& payload)
{
    if (frameWithType.isEmpty())
        return false;
    const quint8 type = static_cast<quint8>(frameWithType.at(0));
    port = (type >> 4) & 0x0F;
    command = type & 0x0F;
    payload = frameWithType.mid(1);
    return true;
}

} // namespace AetherSDR::kiss
