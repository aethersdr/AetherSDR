#include "core/tnc/Ax25.h"

namespace AetherSDR::ax25 {

namespace {

// U-frame control octets with the P/F bit (0x10) cleared. The P/F bit is OR'd
// in separately. These are the canonical AX.25 v2.x values.
constexpr quint8 kPollFinalBit = 0x10;
constexpr quint8 kCtrlUI = 0x03;
constexpr quint8 kCtrlDM = 0x0F;
constexpr quint8 kCtrlSABM = 0x2F;
constexpr quint8 kCtrlDISC = 0x43;
constexpr quint8 kCtrlUA = 0x63;
constexpr quint8 kCtrlFRMR = 0x87;

// S-frame supervisory bits (bits 2-3).
constexpr quint8 kSupRR = 0x00;
constexpr quint8 kSupRNR = 0x04;
constexpr quint8 kSupREJ = 0x08;

// Reserved SSID-octet bits 5-6, conventionally set to 1.
constexpr quint8 kSsidReserved = 0x60;

void appendAddress(QByteArray& out, const Address& addr, bool last, bool cOrHBit)
{
    QString call = addr.call.trimmed().toUpper();
    for (int i = 0; i < 6; ++i) {
        const char ch = (i < call.size()) ? call.at(i).toLatin1() : ' ';
        out.append(static_cast<char>(static_cast<quint8>(ch) << 1));
    }
    quint8 ssidByte = kSsidReserved
        | static_cast<quint8>((addr.ssid & 0x0F) << 1)
        | (last ? 0x01 : 0x00)
        | (cOrHBit ? 0x80 : 0x00);
    out.append(static_cast<char>(ssidByte));
}

std::optional<Address> readAddress(const QByteArray& frame, int offset, bool& lastOut)
{
    if (offset + 7 > frame.size())
        return std::nullopt;

    Address addr;
    for (int i = 0; i < 6; ++i) {
        const char ch = static_cast<char>(
            static_cast<quint8>(frame.at(offset + i)) >> 1);
        if (ch != ' ')
            addr.call.append(QLatin1Char(ch));
    }
    const quint8 ssidByte = static_cast<quint8>(frame.at(offset + 6));
    addr.ssid = (ssidByte >> 1) & 0x0F;
    addr.hasBeenRepeated = (ssidByte & 0x80) != 0;
    addr.commandResponse = (ssidByte & 0x80) != 0;
    lastOut = (ssidByte & 0x01) != 0;
    if (addr.call.isEmpty())
        return std::nullopt;
    return addr;
}

} // namespace

QString Address::toString() const
{
    const QString base = call.trimmed().toUpper();
    if (ssid == 0)
        return base;
    return QStringLiteral("%1-%2").arg(base).arg(ssid);
}

std::optional<Address> Address::parse(const QString& text)
{
    const QString trimmed = text.trimmed().toUpper();
    if (trimmed.isEmpty())
        return std::nullopt;

    Address addr;
    const int dash = trimmed.indexOf(QLatin1Char('-'));
    if (dash < 0) {
        addr.call = trimmed;
        addr.ssid = 0;
    } else {
        addr.call = trimmed.left(dash);
        bool ok = false;
        addr.ssid = trimmed.mid(dash + 1).toInt(&ok);
        if (!ok)
            return std::nullopt;
    }
    if (addr.call.isEmpty() || addr.call.size() > 6 || addr.ssid < 0 || addr.ssid > 15)
        return std::nullopt;
    return addr;
}

QString frameTypeName(FrameType type)
{
    switch (type) {
    case FrameType::I: return QStringLiteral("I");
    case FrameType::RR: return QStringLiteral("RR");
    case FrameType::RNR: return QStringLiteral("RNR");
    case FrameType::REJ: return QStringLiteral("REJ");
    case FrameType::SABM: return QStringLiteral("SABM");
    case FrameType::DISC: return QStringLiteral("DISC");
    case FrameType::DM: return QStringLiteral("DM");
    case FrameType::UA: return QStringLiteral("UA");
    case FrameType::FRMR: return QStringLiteral("FRMR");
    case FrameType::UI: return QStringLiteral("UI");
    case FrameType::Unknown: break;
    }
    return QStringLiteral("?");
}

QByteArray Frame::encode() const
{
    QByteArray out;
    out.reserve(16 + via.size() * 7 + info.size());

    const bool hasVia = !via.isEmpty();
    // Command/response sense lives in the two address C bits (AX.25 v2.x):
    // command frame  -> dest C=1, src C=0; response frame -> dest C=0, src C=1.
    appendAddress(out, dest, /*last=*/false, /*cOrH=*/command);
    appendAddress(out, src, /*last=*/!hasVia, /*cOrH=*/!command);
    for (int i = 0; i < via.size(); ++i) {
        const bool last = (i == via.size() - 1);
        appendAddress(out, via.at(i), last, via.at(i).hasBeenRepeated);
    }

    quint8 control = 0;
    bool carriesPid = false;
    switch (type) {
    case FrameType::I:
        control = static_cast<quint8>((nr & 0x07) << 5)
            | (pollFinal ? kPollFinalBit : 0)
            | static_cast<quint8>((ns & 0x07) << 1);
        carriesPid = true;
        break;
    case FrameType::RR:
    case FrameType::RNR:
    case FrameType::REJ: {
        const quint8 sup = (type == FrameType::RR) ? kSupRR
            : (type == FrameType::RNR) ? kSupRNR : kSupREJ;
        control = static_cast<quint8>((nr & 0x07) << 5)
            | (pollFinal ? kPollFinalBit : 0) | sup | 0x01;
        break;
    }
    case FrameType::UI:
        control = kCtrlUI | (pollFinal ? kPollFinalBit : 0);
        carriesPid = true;
        break;
    case FrameType::SABM: control = kCtrlSABM | (pollFinal ? kPollFinalBit : 0); break;
    case FrameType::DISC: control = kCtrlDISC | (pollFinal ? kPollFinalBit : 0); break;
    case FrameType::DM:   control = kCtrlDM | (pollFinal ? kPollFinalBit : 0); break;
    case FrameType::UA:   control = kCtrlUA | (pollFinal ? kPollFinalBit : 0); break;
    case FrameType::FRMR: control = kCtrlFRMR | (pollFinal ? kPollFinalBit : 0); break;
    case FrameType::Unknown: control = kCtrlUI; carriesPid = true; break;
    }
    out.append(static_cast<char>(control));

    if (carriesPid) {
        out.append(static_cast<char>(pid));
        out.append(info);
    } else if (type == FrameType::FRMR) {
        out.append(info); // FRMR carries a 3-byte reason field
    }
    return out;
}

std::optional<Frame> Frame::decode(const QByteArray& rawNoFcs)
{
    // Minimum: dest(7) + src(7) + control(1).
    if (rawNoFcs.size() < 15)
        return std::nullopt;

    Frame frame;
    bool last = false;
    int offset = 0;

    auto dest = readAddress(rawNoFcs, offset, last);
    if (!dest)
        return std::nullopt;
    const bool destC = dest->commandResponse;
    frame.dest = *dest;
    offset += 7;
    if (last) // dest must not be the last address
        return std::nullopt;

    auto src = readAddress(rawNoFcs, offset, last);
    if (!src)
        return std::nullopt;
    const bool srcC = src->commandResponse;
    frame.src = *src;
    offset += 7;

    int guard = 0;
    while (!last && guard < 8) {
        auto via = readAddress(rawNoFcs, offset, last);
        if (!via)
            return std::nullopt;
        frame.via.append(*via);
        offset += 7;
        ++guard;
    }

    if (offset >= rawNoFcs.size())
        return std::nullopt;
    const quint8 control = static_cast<quint8>(rawNoFcs.at(offset++));

    // AX.25 v2.x command/response: command frame has dest C=1, src C=0.
    frame.command = destC && !srcC ? true : (!destC && srcC ? false : true);
    frame.pollFinal = (control & kPollFinalBit) != 0;

    bool carriesPid = false;
    if ((control & 0x01) == 0x00) {
        frame.type = FrameType::I;
        frame.ns = (control >> 1) & 0x07;
        frame.nr = (control >> 5) & 0x07;
        carriesPid = true;
    } else if ((control & 0x03) == 0x01) {
        frame.nr = (control >> 5) & 0x07;
        const quint8 sup = control & 0x0C;
        frame.type = (sup == kSupRR) ? FrameType::RR
            : (sup == kSupRNR) ? FrameType::RNR
            : (sup == kSupREJ) ? FrameType::REJ : FrameType::Unknown;
    } else { // U frame
        const quint8 modifier = control & ~kPollFinalBit;
        switch (modifier) {
        case kCtrlUI: frame.type = FrameType::UI; carriesPid = true; break;
        case kCtrlDM: frame.type = FrameType::DM; break;
        case kCtrlSABM: frame.type = FrameType::SABM; break;
        case kCtrlDISC: frame.type = FrameType::DISC; break;
        case kCtrlUA: frame.type = FrameType::UA; break;
        case kCtrlFRMR:
            frame.type = FrameType::FRMR;
            frame.info = rawNoFcs.mid(offset);
            break;
        default: frame.type = FrameType::Unknown; break;
        }
    }

    if (carriesPid) {
        if (offset >= rawNoFcs.size())
            return std::nullopt;
        frame.pid = static_cast<quint8>(rawNoFcs.at(offset++));
        frame.info = rawNoFcs.mid(offset);
    }
    return frame;
}

Frame Frame::makeU(const Address& dest, const Address& src, FrameType u,
                   bool pollFinal, bool command)
{
    Frame f;
    f.dest = dest;
    f.src = src;
    f.type = u;
    f.pollFinal = pollFinal;
    f.command = command;
    return f;
}

Frame Frame::makeS(const Address& dest, const Address& src, FrameType s,
                   int nr, bool pollFinal, bool command)
{
    Frame f;
    f.dest = dest;
    f.src = src;
    f.type = s;
    f.nr = nr & 0x07;
    f.pollFinal = pollFinal;
    f.command = command;
    return f;
}

Frame Frame::makeI(const Address& dest, const Address& src, int ns, int nr,
                   bool pollFinal, const QByteArray& info, quint8 pid)
{
    Frame f;
    f.dest = dest;
    f.src = src;
    f.type = FrameType::I;
    f.ns = ns & 0x07;
    f.nr = nr & 0x07;
    f.pollFinal = pollFinal;
    f.command = true; // I frames are always commands
    f.pid = pid;
    f.info = info;
    return f;
}

Frame Frame::makeUI(const Address& dest, const Address& src,
                    const QVector<Address>& via, const QByteArray& info, quint8 pid)
{
    Frame f;
    f.dest = dest;
    f.src = src;
    f.via = via;
    f.type = FrameType::UI;
    f.pollFinal = false;
    f.command = true;
    f.pid = pid;
    f.info = info;
    return f;
}

} // namespace AetherSDR::ax25
