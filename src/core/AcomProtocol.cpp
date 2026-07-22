#include "AcomProtocol.h"

#include <QMap>

namespace AetherSDR {
namespace Acom {

namespace {

quint16 le16(const QByteArray& d, int offset)
{
    return static_cast<quint16>(static_cast<quint8>(d.at(offset)))
         | (static_cast<quint16>(static_cast<quint8>(d.at(offset + 1))) << 8);
}

// LPF band names indexed by the telemetry frame's 4-bit "active FLP channel"
// field (Byte 69 low nibble). 16 entries — indices beyond the spec's
// documented 0..10 are reserved/unused on hardware seen so far.
const QStringList& bandTable()
{
    static const QStringList table = {
        "?m", "160m", "80m", "40/60m", "30m", "20m",
        "17m", "15m", "12m", "10m", "6m", "4m",
        "?m", "?m", "?m", "?m",
    };
    return table;
}

// 600S is the only row verified against real hardware. The others are
// derived, not measured — methodology (design doc §6):
//   - Nominal (rated output): ACOM's own current published spec sheets
//     (acom-bg.com product pages), NOT the model name and NOT the public
//     reference client's constants. Both of those turned out to be wrong
//     for the higher-power models: the model name is not the rated wattage
//     (1200S is rated 1000W, 2020S is rated 1500W), and the reference
//     client's embedded constants (1200S=1200W, 2020S=1800W) don't match
//     ACOM's current official specs either.
//   - Max (gauge red-zone ceiling): where the model name is exactly
//     "rated + 200W" (1200S, 1400S), the name itself is used as a round-
//     number ceiling. Where it isn't (500S/600S/700S have name == rated;
//     2020S's name doesn't fit the +200W pattern at all), the ceiling is
//     the 600S's own confirmed max/nominal ratio (700/600, rounded) applied
//     to that model's rated wattage instead.
//   - Reflected power (nominal/max): no public source exists at all: derived
//     proportionally from the 600S's confirmed reflected/forward ratios
//     (114/600 nominal, 150/700 max) applied to each model's forward figures.
//   - Temperature offset / hasPam2: carried from the reference client where
//     no better source exists; defaulted to the majority value (282 / no
//     PAM2 assumption based on power class) for 1400S, which the reference
//     client doesn't cover at all.
// None of this is a substitute for real confirmation. See AcomConnection's
// auto-ranging (tierForForwardPower) for how the UI stays correct even when
// these constants are wrong, and modelNameForAmplifierType for how little of
// the identification problem the wire protocol itself actually solves.
const QMap<QString, ModelSpec>& modelTable()
{
    static const QMap<QString, ModelSpec> table = {
        {"500S",  ModelSpec{"500S",   500.0f,  600.0f,  95.0f, 129.0f, 282, false}},
        {"600S",  ModelSpec{"600S",   600.0f,  700.0f, 114.0f, 150.0f, 273, false}},
        {"700S",  ModelSpec{"700S",   700.0f,  800.0f, 133.0f, 171.0f, 282, false}},
        {"1200S", ModelSpec{"1200S", 1000.0f, 1200.0f, 190.0f, 257.0f, 281, true}},
        {"1400S", ModelSpec{"1400S", 1200.0f, 1400.0f, 228.0f, 300.0f, 282, true}},
        {"2020S", ModelSpec{"2020S", 1500.0f, 1750.0f, 285.0f, 375.0f, 282, true}},
    };
    return table;
}

// Ascending power order — both by nominal and by max (the two orderings
// agree), so one list drives auto-ranging.
const QStringList& tierOrder()
{
    static const QStringList order = {"500S", "600S", "700S", "1200S", "1400S", "2020S"};
    return order;
}

}  // namespace

QString modeName(Mode mode)
{
    switch (mode) {
        case Mode::Reset:     return QStringLiteral("RESET");
        case Mode::Init:      return QStringLiteral("INIT");
        case Mode::Debug:     return QStringLiteral("DEBUG");
        case Mode::Service:   return QStringLiteral("SERVICE");
        case Mode::Standby:   return QStringLiteral("STANDBY");
        case Mode::OperateRx: return QStringLiteral("OPR/RX");
        case Mode::OperateTx: return QStringLiteral("OPR/TX");
        case Mode::Atac:      return QStringLiteral("ATAC");
        case Mode::SetParams: return QStringLiteral("SET PARAMS");
        case Mode::PowerOff:  return QStringLiteral("OFF");
        default:              return QStringLiteral("UNKNOWN");
    }
}

quint8 checksum(const QByteArray& frameWithoutChecksum)
{
    quint8 sum = 0;
    for (char c : frameWithoutChecksum)
        sum = static_cast<quint8>(sum - static_cast<quint8>(c));
    return sum;
}

QByteArray buildFrame(quint8 address, const QByteArray& data)
{
    QByteArray frame;
    frame.append(static_cast<char>(kStartByte));
    frame.append(static_cast<char>(address));
    frame.append(static_cast<char>(4 + data.size()));  // start+address+length+checksum + data
    frame.append(data);
    frame.append(static_cast<char>(checksum(frame)));
    return frame;
}

void FrameParser::feed(const QByteArray& bytes)
{
    m_buf.append(bytes);

    while (true) {
        int start = m_buf.indexOf(static_cast<char>(kStartByte));
        if (start < 0) {
            m_buf.clear();
            return;
        }
        if (start > 0)
            m_buf.remove(0, start);

        if (m_buf.size() < 3)
            return;  // need start+address+length to know how big this frame is

        const quint8 length = static_cast<quint8>(m_buf.at(2));
        if (length < 4 || length > kMaxFrameLength) {
            // Not a real frame length — this 0x55 was noise (or a bit-flip
            // produced an implausible size). Resync to the next 0x55 rather than
            // buffering toward a length that will never resolve.
            if (!resyncToNextStart())
                return;
            continue;
        }
        if (m_buf.size() < length)
            return;  // wait for the rest of the frame to arrive

        const QByteArray candidate = m_buf.left(length);
        const quint8 expected = checksum(candidate.left(length - 1));
        if (expected == static_cast<quint8>(candidate.at(length - 1))) {
            Frame f;
            f.address = static_cast<quint8>(candidate.at(1));
            f.data = candidate.mid(3, length - 4);
            if (m_onFrame)
                m_onFrame(f);
            m_buf.remove(0, length);
        } else {
            // Checksum mismatch: either this 0x55 was payload data, not a real
            // start byte, or the frame was corrupted in transit. Resync to the
            // next 0x55 — a genuine frame's start byte may still be later.
            if (!resyncToNextStart())
                return;
        }
    }
}

bool FrameParser::resyncToNextStart()
{
    const int next = m_buf.indexOf(static_cast<char>(kStartByte), 1);
    if (next < 0) {
        m_buf.clear();
        return false;
    }
    m_buf.remove(0, next);
    return true;
}

std::optional<Telemetry> decodeTelemetry(const QByteArray& payload)
{
    if (payload.size() < 68)
        return std::nullopt;

    Telemetry t;
    t.mode                   = static_cast<Mode>((static_cast<quint8>(payload.at(0)) & 0xF0) >> 4);
    t.dcPowerPam1_x10W       = le16(payload, 5);
    {
        const quint16 hw = le16(payload, 9);
        const quint16 lw = le16(payload, 11);
        t.systemClockSec = (static_cast<quint32>(hw) << 16) | lw;
    }
    t.paTempRaw              = le16(payload, 13);
    t.inputPower_x10W        = le16(payload, 17);
    t.forwardPowerW          = le16(payload, 19);
    t.reflectedPowerW        = le16(payload, 21);
    t.swr_x100                = le16(payload, 23);
    t.dissipationPam1_x10W   = le16(payload, 25);
    t.vcc5_mV                 = le16(payload, 33);
    t.vcc26_x10V              = le16(payload, 35);
    t.hv1_x10V                = le16(payload, 37);
    t.id1_mA                  = le16(payload, 41);
    t.carrierFreqKHz          = le16(payload, 45);
    t.errorCode               = static_cast<quint8>(payload.at(63));
    t.errorParam               = le16(payload, 64);
    {
        const quint8 fanBand = static_cast<quint8>(payload.at(66));
        t.fanSpeed   = (fanBand & 0xF0) >> 4;
        t.activeBand = fanBand & 0x0F;
    }
    return t;
}

QString bandName(int index)
{
    const auto& table = bandTable();
    if (index < 0 || index >= table.size())
        return QStringLiteral("?m");
    return table.at(index);
}

QString errorCodeName(quint8 code)
{
    switch (code) {
        case 0xFF: return QStringLiteral("No fault");
        case 0x00:
        case 0x08: return QStringLiteral("Hot switching");
        case 0x03: return QStringLiteral("Drive power at wrong time");
        case 0x04:
        case 0x05: return QStringLiteral("Reflected power warning");
        case 0x06:
        case 0x07: return QStringLiteral("Drive power too high");
        case 0x0C: return QStringLiteral("RF power at wrong time");
        case 0x0E: return QStringLiteral("Stop transmission first");
        case 0x0F: return QStringLiteral("Remove drive power");
        case 0x24:
        case 0x25:
        case 0x39:
        case 0x44:
        case 0x45:
        case 0x59: return QStringLiteral("Excessive PAM current");
        case 0x70: return QStringLiteral("CAT error");
        default:   return QStringLiteral("Fault — see amplifier display");
    }
}

QByteArray buildModeCommand(ModeCommand cmd)
{
    QByteArray data;
    data.append(static_cast<char>(0x02));  // sub-command: request amplifier mode change
    data.append(static_cast<char>(0x00));  // param (HW) — not used for mode change
    data.append(static_cast<char>(static_cast<quint8>(cmd)));  // param (LW) — desired mode
    data.append(static_cast<char>(0x00));  // keyboard state — not applicable from software
    return buildFrame(static_cast<quint8>(Address::Command), data);
}

QByteArray buildClearFaultsCommand()
{
    QByteArray data;
    data.append(static_cast<char>(0x08));  // sub-command: clear soft faults
    data.append(static_cast<char>(0x00));
    data.append(static_cast<char>(0x00));
    data.append(static_cast<char>(0x00));
    return buildFrame(static_cast<quint8>(Address::Command), data);
}

QByteArray buildTelemetryEnable()
{
    return buildFrame(static_cast<quint8>(Address::TelemetryEnable));
}

QByteArray buildTelemetryDisable()
{
    return buildFrame(static_cast<quint8>(Address::TelemetryDisable));
}

QByteArray buildAck(quint8 receivedMessageNumber)
{
    QByteArray data;
    data.append(static_cast<char>(receivedMessageNumber));
    return buildFrame(static_cast<quint8>(Address::Ack), data);
}

QByteArray buildRequestMessage(quint8 desiredAddress)
{
    QByteArray data;
    data.append(static_cast<char>(desiredAddress));
    return buildFrame(static_cast<quint8>(Address::RequestMessage), data);
}

const ModelSpec& modelSpec(const QString& modelName)
{
    const auto& table = modelTable();
    auto it = table.constFind(modelName);
    if (it != table.constEnd())
        return it.value();
    return table.constFind(QStringLiteral("600S")).value();  // safe fallback
}

QStringList modelNames()
{
    return modelTable().keys();
}

QStringList orderedModelTiers()
{
    return tierOrder();
}

QString tierForForwardPower(float watts)
{
    const auto& order = tierOrder();
    // Small margin so a reading right at a tier's ceiling doesn't immediately
    // demand the next tier up — avoids boundary flapping on ordinary peaks.
    constexpr float kMargin = 1.02f;
    for (const QString& name : order) {
        if (watts <= modelSpec(name).maxForwardW * kMargin)
            return name;
    }
    return order.last();  // exceeds even the top tier — clamp, don't guess higher
}

std::optional<SystemConfig> decodeSystemConfig(const QByteArray& payload)
{
    if (payload.size() < 26)
        return std::nullopt;

    SystemConfig cfg;
    cfg.amplifierType = static_cast<quint8>(payload.at(0));
    cfg.fwVersion     = static_cast<quint8>(payload.at(1));
    cfg.fwSubVersion  = static_cast<quint8>(payload.at(2));

    QByteArray serial = payload.mid(13, 12);
    cfg.serialNumberHex = QString::fromLatin1(serial.toHex());
    cfg.hardFaultCount = static_cast<quint8>(payload.at(25));
    return cfg;
}

QString modelNameForAmplifierType(quint8 amplifierType)
{
    // Only type 1 is documented by the spec ("1 - A600S"). Every other
    // value is a genuine unknown, not a guess dressed up as one — see the
    // modelTable() comment and design doc §6 for why the rest of the line
    // is covered by auto-ranging instead of a type-code table.
    if (amplifierType == 1)
        return QStringLiteral("600S");
    return QString();
}

}  // namespace Acom
}  // namespace AetherSDR
