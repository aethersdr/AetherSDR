#include "SpeProtocol.h"

#include <QMap>

namespace AetherSDR {
namespace Spe {

namespace {

quint16 dataSum(const QByteArray& data)
{
    quint16 sum = 0;
    for (char c : data)
        sum = static_cast<quint16>(sum + static_cast<quint8>(c));
    return sum;
}

const QStringList& bandTable()
{
    static const QStringList table = {
        "160m", "80m", "60m", "40m", "30m", "20m",
        "17m", "15m", "12m", "10m", "6m", "4m",
    };
    return table;
}

const QMap<QString, ModelSpec>& modelTable()
{
    // 1.5K-FA thresholds (1450/1500/1600) are carried from a field-proven
    // control application for that exact model (see the design note); the
    // 1.3K-FA and 2K-FA rows apply the same rated/rated±/rated+100 shape to
    // SPE's published rated output for those models. Memory banks: the spec
    // documents A/B for the 1.3K-FA (and the 1.5K-FA reports them in the
    // same field); the 2K-FA always reads "x". Combiner/lower-heatsink
    // temperatures are only real on the 2K-FA (spec §5's field table).
    // LOW/MID nominals: 1.5K-FA's 500/1000 are hardware-validated (the
    // field-proven control application rescaled its bar to exactly those on
    // level changes); 1.3K-FA and 2K-FA LOW/MID are derived, not measured.
    static const QMap<QString, ModelSpec> table = {
        {"13K", ModelSpec{"13K", "1.3K-FA", 1300.0f, 1400.0f, 1250.0f,  500.0f, 1000.0f, true,  false}},
        {"15K", ModelSpec{"15K", "1.5K-FA", 1500.0f, 1600.0f, 1450.0f,  500.0f, 1000.0f, true,  false}},
        {"20K", ModelSpec{"20K", "2K-FA",   2000.0f, 2100.0f, 1950.0f, 1000.0f, 1500.0f, false, true}},
    };
    return table;
}

}  // namespace

QByteArray buildPacket(const QByteArray& data)
{
    QByteArray packet;
    packet.append(static_cast<char>(kHostSync));
    packet.append(static_cast<char>(kHostSync));
    packet.append(static_cast<char>(kHostSync));
    packet.append(static_cast<char>(data.size()));
    packet.append(data);
    packet.append(static_cast<char>(dataSum(data) & 0xFF));
    // Trailing CR LF — required in practice on real hardware (see the
    // header comment); harmless bytes outside the framed packet otherwise.
    packet.append('\r');
    packet.append('\n');
    return packet;
}

QByteArray buildKeyCommand(Key key)
{
    return buildPacket(QByteArray(1, static_cast<char>(static_cast<quint8>(key))));
}

QByteArray buildStatusRequest()
{
    return buildPacket(QByteArray(1, static_cast<char>(kStatusRequest)));
}

QByteArray buildBacklightCommand(bool on)
{
    return buildPacket(QByteArray(1, static_cast<char>(on ? kBacklightOn : kBacklightOff)));
}

void FrameParser::feed(const QByteArray& bytes)
{
    m_buf.append(bytes);

    while (true) {
        int start = m_buf.indexOf(static_cast<char>(kAmpSync));
        if (start < 0) {
            m_buf.clear();
            return;
        }
        if (start > 0)
            m_buf.remove(0, start);

        if (m_buf.size() < 4)
            return;  // need the full sync run + CNT to know the frame size

        // All three sync bytes must be present; a lone 0xAA inside payload
        // data is common (it's a legal ASCII-adjacent byte), so anything
        // short of the full run is a false start.
        if (static_cast<quint8>(m_buf.at(1)) != kAmpSync
            || static_cast<quint8>(m_buf.at(2)) != kAmpSync) {
            if (!resyncToNextSync())
                return;
            continue;
        }

        const quint8 cnt = static_cast<quint8>(m_buf.at(3));
        if (cnt == 0 || cnt > kMaxDataLength) {
            if (!resyncToNextSync())
                return;
            continue;
        }

        // The two reply shapes the spec defines checksum differently: the
        // single-byte ACK carries a 1-byte checksum; everything longer (in
        // practice, the 67-byte Status string) carries a 16-bit checksum
        // (low, then high byte). CR LF after the Status checksum is not
        // consumed here — the next sync scan skips it naturally.
        const int checksumBytes = (cnt == 1) ? 1 : 2;
        const int frameSize = 4 + cnt + checksumBytes;
        if (m_buf.size() < frameSize)
            return;  // wait for the rest of the frame

        const QByteArray data = m_buf.mid(4, cnt);
        const quint16 sum = dataSum(data);
        bool valid = static_cast<quint8>(m_buf.at(4 + cnt)) == (sum & 0xFF);
        if (valid && checksumBytes == 2)
            valid = static_cast<quint8>(m_buf.at(5 + cnt)) == ((sum >> 8) & 0xFF);

        if (valid) {
            Frame f;
            f.data = data;
            if (m_onFrame)
                m_onFrame(f);
            m_buf.remove(0, frameSize);
        } else {
            if (!resyncToNextSync())
                return;
        }
    }
}

bool FrameParser::resyncToNextSync()
{
    const int next = m_buf.indexOf(static_cast<char>(kAmpSync), 1);
    if (next < 0) {
        m_buf.clear();
        return false;
    }
    m_buf.remove(0, next);
    return true;
}

std::optional<Status> parseStatus(const QByteArray& payload)
{
    const QString text = QString::fromLatin1(payload);
    QStringList parts = text.split(u',');
    for (QString& p : parts)
        p = p.trimmed();

    // 20 tokens with the leading marker character, 19 without — accept both
    // so a firmware that drops the marker still parses (see header).
    if (parts.size() >= 20)
        parts.removeFirst();
    if (parts.size() < 19)
        return std::nullopt;

    Status s;
    s.id           = parts.at(0);
    s.operate      = parts.at(1) == QStringLiteral("O");
    s.transmitting = parts.at(2) == QStringLiteral("T");
    if (!parts.at(3).isEmpty())
        s.bank = parts.at(3).at(0);
    s.input     = parts.at(4).toInt();
    s.bandIndex = parts.at(5).toInt();

    // TX antenna field packs a digit and an optional ATU state letter
    // ("1a" = antenna 1, ATU enabled).
    const QString txAnt = parts.at(6);
    QString antDigits;
    for (QChar c : txAnt) {
        if (c.isDigit())
            antDigits.append(c);
        else if (c == u't' || c == u'b' || c == u'a')
            s.atuState = c;
    }
    s.txAntenna = antDigits.toInt();

    s.rxAntenna = parts.at(7);
    if (!parts.at(8).isEmpty())
        s.powerLevel = parts.at(8).at(0);
    s.outputPowerW = parts.at(9).toFloat();
    s.swrAtu       = parts.at(10).toFloat();
    s.swrAnt       = parts.at(11).toFloat();
    s.paVoltageV   = parts.at(12).toFloat();
    s.paCurrentA   = parts.at(13).toFloat();
    s.tempUpper    = parts.at(14).toInt();
    s.tempLower    = parts.at(15).toInt();
    s.tempCombiner = parts.at(16).toInt();
    if (!parts.at(17).isEmpty())
        s.warning = parts.at(17).at(0);
    if (!parts.at(18).isEmpty())
        s.alarm = parts.at(18).at(0);
    return s;
}

QString bandName(int index)
{
    const auto& table = bandTable();
    if (index < 0 || index >= table.size())
        return QStringLiteral("?m");
    return table.at(index);
}

QString warningText(QChar code)
{
    switch (code.unicode()) {
        case u'N': return QString();  // no warnings — empty so banners can hide
        case u'M': return QStringLiteral("Amplifier alarm");
        case u'A': return QStringLiteral("No antenna selected");
        case u'S': return QStringLiteral("Antenna SWR");
        case u'B': return QStringLiteral("No valid band");
        case u'P': return QStringLiteral("Power limit exceeded");
        case u'O': return QStringLiteral("Overheating");
        case u'Y': return QStringLiteral("ATU not available");
        case u'W': return QStringLiteral("Tuning with no power");
        case u'K': return QStringLiteral("ATU bypassed");
        case u'R': return QStringLiteral("Power switch held by remote");
        case u'T': return QStringLiteral("Combiner overheating");
        case u'C': return QStringLiteral("Combiner fault");
        // Unknown code: echo the raw letter — the caller already prefixes
        // severity ("Warning: ..."), so returning a labeled string here
        // would double it.
        default:   return QString(code);
    }
}

QString alarmText(QChar code)
{
    switch (code.unicode()) {
        case u'N': return QString();  // no alarms
        case u'S': return QStringLiteral("SWR exceeding limits");
        case u'A': return QStringLiteral("Amplifier protection");
        case u'D': return QStringLiteral("Input overdriving");
        case u'H': return QStringLiteral("Excess overheating");
        case u'C': return QStringLiteral("Combiner fault");
        default:   return QString(code);  // same no-double-prefix rule as warningText
    }
}

QString powerLevelName(QChar code)
{
    switch (code.unicode()) {
        case u'L': return QStringLiteral("LOW");
        case u'M': return QStringLiteral("MID");
        case u'H': return QStringLiteral("HIGH");
        // Empty for a null QChar (Status::powerLevel's not-yet-reported
        // default) so the GUI's "PWR" fallback engages instead of a stray
        // NUL glyph; unknown real letters still echo through.
        default:   return code.isNull() ? QString() : QString(code);
    }
}

namespace Rfc2217 {

namespace {
constexpr quint8 kIac = 0xFF;
constexpr quint8 kWill = 0xFB;
constexpr quint8 kSb = 0xFA;
constexpr quint8 kSe = 0xF0;
constexpr quint8 kDo = 0xFD;
constexpr quint8 kDont = 0xFE;
constexpr quint8 kComPortOption = 0x2C;
constexpr quint8 kSetControl = 0x05;
}  // namespace

QByteArray buildWillComPortOption()
{
    QByteArray b;
    b.append(static_cast<char>(kIac));
    b.append(static_cast<char>(kWill));
    b.append(static_cast<char>(kComPortOption));
    return b;
}

QByteArray buildSetControl(quint8 ctrl)
{
    QByteArray b;
    b.append(static_cast<char>(kIac));
    b.append(static_cast<char>(kSb));
    b.append(static_cast<char>(kComPortOption));
    b.append(static_cast<char>(kSetControl));
    b.append(static_cast<char>(ctrl));
    b.append(static_cast<char>(kIac));
    b.append(static_cast<char>(kSe));
    return b;
}

OptionReply scanComPortOptionReply(const QByteArray& bytes)
{
    OptionReply result = OptionReply::None;
    for (int i = 0; i + 2 < bytes.size(); ++i) {
        if (static_cast<quint8>(bytes.at(i)) != kIac)
            continue;
        if (static_cast<quint8>(bytes.at(i + 2)) != kComPortOption)
            continue;
        const quint8 verb = static_cast<quint8>(bytes.at(i + 1));
        if (verb == kDo)
            result = OptionReply::Accepted;
        else if (verb == kDont)
            result = OptionReply::Refused;
    }
    return result;
}

}  // namespace Rfc2217

const ModelSpec& modelSpec(const QString& id)
{
    const auto& table = modelTable();
    auto it = table.constFind(id);
    if (it != table.constEnd())
        return it.value();
    // Unknown ID (e.g. an older/newer model outside the spec's scope) —
    // fall back to the hardware-validated 1.5K-FA entry.
    return table.constFind(QStringLiteral("15K")).value();
}

QStringList modelIds()
{
    return modelTable().keys();
}

float levelNominalW(const ModelSpec& spec, QChar level)
{
    switch (level.unicode()) {
        case u'L': return spec.lowNominalW;
        case u'M': return spec.midNominalW;
        default:   return spec.nominalPowerW;  // H, or unknown — full scale
    }
}

namespace {

// The gauge shape carried from the field-proven 1.5K-FA control application:
// yellow from nominal−50 W, ceiling at nominal+100 W. Applied to the LOW/MID
// nominals, which have no tabulated thresholds of their own; every model
// row's HIGH figures already encode the same shape.
constexpr float kWarnMarginW    = 50.0f;
constexpr float kCeilingMarginW = 100.0f;

GaugeRange derivedRange(float nominalW)
{
    return {nominalW, nominalW - kWarnMarginW, nominalW + kCeilingMarginW};
}

}  // namespace

GaugeRange levelGaugeRange(const ModelSpec& spec, QChar level)
{
    switch (level.unicode()) {
        case u'L': return derivedRange(spec.lowNominalW);
        case u'M': return derivedRange(spec.midNominalW);
        // H, or unknown — the model row's own figures, verbatim, so an edit
        // to modelTable() reaches the bar.
        default:   return {spec.nominalPowerW, spec.warnPowerW, spec.maxPowerW};
    }
}

}  // namespace Spe
}  // namespace AetherSDR
