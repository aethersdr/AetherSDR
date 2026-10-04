#include "SpeLegacyProtocol.h"

#include <algorithm>
#include <cmath>

#include <QString>

namespace AetherSDR {
namespace Spe {
namespace Legacy {

namespace {

quint8 dataSum(const QByteArray& data)
{
    quint8 sum = 0;
    for (char c : data) {
        sum = static_cast<quint8>(sum + static_cast<quint8>(c));
    }
    return sum;
}

quint16 le16(const QByteArray& d, int offset)
{
    return static_cast<quint16>(static_cast<quint8>(d.at(offset))
        | (static_cast<quint8>(d.at(offset + 1)) << 8));
}

// Status payload offsets (payload index 0 = the STATUS_CODE byte).
constexpr int kFlags       = 1;
constexpr int kDisplayCtx  = 2;
constexpr int kBandInput   = 14;
constexpr int kCatAntenna  = 18;
constexpr int kSwrOrGain   = 19;
constexpr int kTemperature = 21;
constexpr int kPaOut       = 22;
constexpr int kReverse     = 24;
constexpr int kVoltage     = 26;
constexpr int kCurrent     = 28;

constexpr quint8 kFlagOperate = 0x02;
constexpr quint8 kFlagTx      = 0x04;
constexpr quint8 kFlagAlarm   = 0x08;
constexpr quint8 kFlagFull    = 0x10;

// STANDBY SWR value the amplifier sends for "infinite".
constexpr quint16 kSwrInfinite = 9999;
constexpr float kSwrCap = 99.9f;

// The 1K-FA covers 160..6 m without 60 m; the shared table has 60 m at 2.
int sharedBandIndex(int legacyBand)
{
    return legacyBand <= 1 ? legacyBand : legacyBand + 1;
}

QString displayContextWarning(quint8 ctx)
{
    switch (ctx) {
        case 0x11: return QStringLiteral("Supply voltage low (HALF)");
        case 0x12: return QStringLiteral("Supply voltage low (FULL)");
        case 0x13: return QStringLiteral("Supply voltage high (HALF)");
        case 0x14: return QStringLiteral("Supply voltage high (FULL)");
        case 0x15: return QStringLiteral("Supply current high (HALF)");
        case 0x16: return QStringLiteral("Supply current high (FULL)");
        case 0x17: return QStringLiteral("Temperature high");
        case 0x18: return QStringLiteral("Input power too high");
        case 0x1B: return QStringLiteral("Reverse power high");
        case 0x1C: return QStringLiteral("PA protection activated");
        case 0x1E: return QStringLiteral("Shutdown in progress");
        default:   return QString();
    }
}

}  // namespace

QByteArray buildPacket(const QByteArray& data)
{
    QByteArray p;
    p.reserve(data.size() + 5);
    p.append(3, static_cast<char>(kHostSync));
    p.append(static_cast<char>(data.size()));
    p.append(data);
    p.append(static_cast<char>(dataSum(data)));
    return p;
}

QByteArray buildRcuOn()
{
    return buildPacket(QByteArray(1, static_cast<char>(kRcuOn)));
}

QByteArray buildRcuOff()
{
    return buildPacket(QByteArray(1, static_cast<char>(kRcuOff)));
}

quint8 keyCode(Key key)
{
    switch (key) {
        case Key::Input:      return 0x28;
        case Key::BandDown:   return 0x29;
        case Key::BandUp:     return 0x2A;
        case Key::Antenna:    return 0x2B;
        case Key::Cat:        return 0x2C;
        case Key::LeftArrow:  return 0x2D;
        case Key::RightArrow: return 0x2E;
        case Key::Set:        return 0x2F;
        case Key::LMinus:     return 0x30;
        case Key::LPlus:      return 0x31;
        case Key::CMinus:     return 0x32;
        case Key::CPlus:      return 0x33;
        case Key::Tune:       return 0x34;
        case Key::SwitchOff:  return 0x18;
        case Key::Power:      return 0x1A;  // MODE: HALF <-> FULL
        case Key::Display:    return 0x1B;
        case Key::Operate:    return 0x1C;
    }
    return 0x00;
}

QByteArray buildKeyCommand(Key key)
{
    QByteArray data;
    data.append(static_cast<char>(kKeyOn));
    data.append(static_cast<char>(keyCode(key)));
    return buildPacket(data);
}

void FrameParser::feed(const QByteArray& bytes)
{
    m_buf.append(bytes);
    const QByteArray sync(3, static_cast<char>(kAmpSync));

    for (;;) {
        const qsizetype start = m_buf.indexOf(sync);
        if (start < 0) {
            // Keep a trailing partial sync so a split sequence still matches.
            const qsizetype keep = std::min<qsizetype>(m_buf.size(), 2);
            m_buf = m_buf.right(keep);
            return;
        }
        if (start > 0) {
            m_buf.remove(0, start);
        }
        if (m_buf.size() < 4) {
            return;
        }
        const int cnt = static_cast<quint8>(m_buf.at(3));
        if (cnt != 1 && cnt != kStatusDataLength) {
            m_buf.remove(0, 1);  // not a frame this protocol defines — resync
            continue;
        }
        const int frameLen = 4 + cnt + 1;
        if (m_buf.size() < frameLen) {
            return;
        }
        const QByteArray data = m_buf.mid(4, cnt);
        if (static_cast<quint8>(m_buf.at(4 + cnt)) != dataSum(data)) {
            m_buf.remove(0, 1);
            continue;
        }
        m_buf.remove(0, frameLen);
        if (m_onFrame) {
            m_onFrame(Frame{data});
        }
    }
}

float swrFromPower(float forwardW, float reverseW)
{
    if (forwardW <= 0.0f) {
        return 0.0f;
    }
    if (reverseW >= forwardW) {
        return kSwrCap;
    }
    const float rho = std::sqrt(std::max(reverseW, 0.0f) / forwardW);
    return std::min((1.0f + rho) / (1.0f - rho), kSwrCap);
}

std::optional<Status> parseStatus(const QByteArray& d)
{
    if (d.size() != kStatusDataLength) {
        return std::nullopt;
    }

    Status s;
    s.id = QString::fromLatin1(kModelId);

    const quint8 flags = static_cast<quint8>(d.at(kFlags));
    s.operate = (flags & kFlagOperate) != 0;
    s.transmitting = (flags & kFlagTx) != 0;
    s.powerLevel = (flags & kFlagFull) ? u'H' : u'L';

    const quint8 bandInput = static_cast<quint8>(d.at(kBandInput));
    const int band = (bandInput >> 4) & 0x0F;
    s.bandIndex = band <= 9 ? sharedBandIndex(band) : -1;
    s.input = (bandInput & 0x0F) == 0 ? 1 : 2;

    const int ant = static_cast<quint8>(d.at(kCatAntenna)) & 0x0F;
    s.txAntenna = ant <= 3 ? ant + 1 : 0;
    s.atuState = QChar();
    s.bank = u'x';

    s.outputPowerW = le16(d, kPaOut) / 10.0f;
    const float reverseW = le16(d, kReverse) / 10.0f;
    if (s.operate) {
        s.swrAnt = swrFromPower(s.outputPowerW, reverseW);
    } else {
        const quint16 raw = le16(d, kSwrOrGain);
        s.swrAnt = raw >= kSwrInfinite ? kSwrCap : raw / 100.0f;
    }
    s.swrAtu = 0.0f;

    s.paVoltageV = le16(d, kVoltage) / 10.0f;
    s.paCurrentA = le16(d, kCurrent) / 10.0f;
    s.tempUpper = static_cast<quint8>(d.at(kTemperature));

    s.warningDetail = displayContextWarning(static_cast<quint8>(d.at(kDisplayCtx)));
    if (flags & kFlagAlarm) {
        s.alarmDetail = QStringLiteral("Alarm active — see the amplifier display");
    }
    return s;
}

}  // namespace Legacy
}  // namespace Spe
}  // namespace AetherSDR
