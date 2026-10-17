#include "Kxpa100Protocol.h"
#include <QRegularExpression>
#include <QStringList>
#include <cmath>
#include <limits>

namespace AetherSDR::Kxpa100 {
namespace {
std::optional<int> number(const QByteArray& text, int width, int low, int high, int base = 10)
{
    if (text.size() != width) { return {}; }
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || (base == 16 && c >= 'A' && c <= 'F'))) { return {}; }
    }
    bool ok = false;
    const int value = text.toInt(&ok, base);
    return ok && value >= low && value <= high ? std::optional<int>(value) : std::nullopt;
}
QByteArray bandCode(int band) { return QByteArray::number(band).rightJustified(2, '0'); }
}
void FrameParser::feed(const QByteArray& bytes)
{
    for (const char c : bytes) {
        if (c == ';') {
            const QByteArray frame = m_buffer.trimmed();
            m_buffer.clear();
            const bool accepted = !m_discard;
            m_discard = false;
            if (accepted && m_callback) { m_callback(frame); }
        } else if (c == '\r' || c == '\n') {
            if (!m_discard && !m_buffer.isEmpty() && !m_buffer.endsWith(' ')) { m_buffer.append(' '); }
        } else if (c < 32 || c > 126 || m_buffer.size() >= 512) {
            m_buffer.clear(); m_discard = true;
        } else if (!m_discard) {
            m_buffer.append(c);
        }
    }
}
bool applyStatus(const QByteArray& frame, Status& status)
{
    if (frame == "^IKXPA100") { status.identity = QStringLiteral("KXPA100"); return true; }
    if (frame.startsWith("^RV")) {
        static const QRegularExpression version(QStringLiteral("^\\^RV[0-9]{2}\\.[0-9]{2}$"));
        if (!version.match(QString::fromLatin1(frame)).hasMatch()) { return false; }
        status.firmware = QString::fromLatin1(frame.mid(3)); return true;
    }
    if (frame.startsWith("^SN")) {
        if (!number(frame.mid(3), 5, 0, 99999)) { return false; }
        status.serial = QString::fromLatin1(frame.mid(3)); return true;
    }
    const std::array<std::pair<const char*, std::optional<bool>*>, 4> booleans{{
        {"^OP", &status.operate}, {"^TU", &status.tunerPresent}, {"^TP", &status.tuning}, {"^XP", &status.transceiverPolling}}};
    for (const auto& [prefix, target] : booleans) {
        const QByteArray key(prefix);
        if (frame == key + '0' || frame == key + '1') { *target = frame.endsWith('1'); return true; }
    }
    if (frame == "^MDA" || frame == "^MDM" || frame == "^MDB") {
        status.mode = frame.endsWith('A') ? Mode::Auto : frame.endsWith('M') ? Mode::Manual : Mode::Bypass; return true;
    }
    if (frame == "^BYB" || frame == "^BYN") { status.bypassed = frame.endsWith('B'); return true; }
    if (frame == "^SIT" || frame == "^SIA") {
        status.side = frame.endsWith('T') ? CapacitorSide::Transmitter : CapacitorSide::Antenna; return true;
    }
    struct Integer { const char* prefix; int width, low, high, base; std::optional<int>* target; };
    const std::array<Integer, 7> integers{{
        {"^AN",1,1,2,10,&status.antenna}, {"^BN",2,0,10,10,&status.band},
        {"^AT",1,0,2,10,&status.attenuator}, {"^F",5,0,99999,10,&status.frequencyKhz},
        {"^CR",2,0,255,16,&status.capacitorMask}, {"^LR",2,0,255,16,&status.inductorMask},
        {"^XI",1,0,9,10,&status.transceiverInterface}}};
    for (const Integer& field : integers) {
        const QByteArray prefix(field.prefix);
        if (frame.startsWith(prefix)) {
            if (const auto value = number(frame.mid(prefix.size()), field.width, field.low, field.high, field.base)) {
                *field.target = *value; return true;
            }
        }
    }
    struct Measurement { const char* prefix; int width; double scale; std::optional<double>* target; };
    const std::array<Measurement, 8> measurements{{
        {"^PI",4,10,&status.inputWatts}, {"^PF",4,10,&status.forwardWatts},
        {"^PV",4,10,&status.reflectedWatts}, {"^PD",4,10,&status.dissipatedWatts},
        {"^PC",4,10,&status.drainAmps}, {"^TM",4,10,&status.temperatureC},
        {"^SV",5,1000,&status.supplyVolts}, {"^SB",3,10,&status.bypassSwr}}};
    for (const Measurement& field : measurements) {
        const QByteArray prefix(field.prefix);
        if (frame.startsWith(prefix)) {
            if (const auto value = number(frame.mid(prefix.size()), field.width, 0, field.width == 5 ? 99999 : 9999)) {
                *field.target = *value / field.scale; return true;
            }
        }
    }
    if (frame.startsWith("^SW")) {
        QByteArray digits = frame.mid(3);
        // The documented nn.n format includes zero; compact firmware replies use nnn.
        const bool decimal = digits.size() == 4 && digits[2] == '.';
        if (decimal) { digits.remove(2, 1); }
        const auto value = number(digits, 3, decimal ? 0 : 10, 999);
        if (!value) { return false; }
        status.swr = *value / 10.0; return true;
    }
    if (frame.size() == 4 && frame.startsWith("^AD") && QByteArray("DFIJNV").contains(frame[3])) {
        status.attenuatorReason = frame[3]; return true;
    }
    if (frame.startsWith("^FL") && (frame.size() == 9 || frame == "^FLN")) {
        const char code = frame[3];
        if (!QByteArray("NACDHILPRST").contains(code)) { return false; }
        const auto value = frame == "^FLN" ? std::optional<int>(0) : number(frame.mid(4), 5, 0, 99999);
        if (!value) { return false; }
        status.faultCode = code;
        status.faultDetail = *value / (code == 'N' ? 1.0 : code == 'H' || code == 'L' ? 1000.0 : 10.0);
        return true;
    }
    return false;
}
bool matchesStatusQuery(const QByteArray& query, const QByteArray& frame)
{
    if (query == ";") { return frame.isEmpty(); }
    if (!query.endsWith(';')) { return false; }
    const QByteArray prefix = query.chopped(1);
    if (!frame.startsWith(prefix)) { return false; }
    if (query == "^F;" && (frame.size() != 7 || frame[2] < '0' || frame[2] > '9')) { return false; }
    Status scratch;
    return applyStatus(frame, scratch);
}
QByteArray buildFrequencyCommand(const QByteArray& command, int khz)
{
    if (!isSupportedFrequency(khz) || (command != "^SM" && command != "^MT" && command != "^DM")) { return {}; }
    return command + QByteArray::number(khz).rightJustified(5, '0') + ';';
}
QByteArray buildErase(int band, int antenna)
{
    if (!((band >= 0 && band <= 10) || band == 15) || antenna < 0 || antenna > 2) { return {}; }
    return "^EM" + bandCode(band) + QByteArray::number(antenna) + ';';
}
QByteArray configurationSet(int band, int mask)
{
    return band >= 0 && band < 11 && mask >= 1 && mask <= 3
        ? "^AE" + bandCode(band) + QByteArray::number(mask) + ';' : QByteArray{};
}
std::optional<Configuration> parseConfiguration(const QByteArray& frame)
{
    if (!frame.startsWith("^AEA") || frame.size() != 15) { return {}; }
    Configuration result;
    for (int band = 0; band < 11; ++band) {
        const auto value = number(frame.mid(4 + band, 1), 1, 1, 3);
        if (!value) { return {}; }
        result[band] = *value;
    }
    return result;
}
QString bandName(int band)
{
    static const QStringList names{QStringLiteral("160m"),QStringLiteral("80m"),QStringLiteral("60m"),QStringLiteral("40m"),
        QStringLiteral("30m"),QStringLiteral("20m"),QStringLiteral("17m"),QStringLiteral("15m"),QStringLiteral("12m"),QStringLiteral("10m"),QStringLiteral("6m")};
    return band >= 0 && band < names.size() ? names[band] : QStringLiteral("Unknown band");
}
int relayTotal(bool capacitor, int mask)
{
    constexpr std::array<int, 8> capacitors{10,22,40,82,150,300,660,1360};
    constexpr std::array<int, 8> inductors{50,110,230,480,1000,2100,4400,9000};
    if (mask < 0 || mask > 255) { return 0; }
    int total = 0;
    for (int bit = 0; bit < 8; ++bit) { if (mask & (1 << bit)) { total += (capacitor ? capacitors : inductors)[bit]; } }
    return total;
}
int relayMaskForValue(bool capacitor, int value)
{
    int closest = 0;
    qint64 distance = std::numeric_limits<qint64>::max();
    for (int mask = 0; mask < 256; ++mask) {
        const qint64 delta = std::abs(qint64(relayTotal(capacitor, mask)) - value);
        if (delta < distance) { closest = mask; distance = delta; }
    }
    return closest;
}
QString currentFaultName(char code)
{
    switch (code) {
    case 'N': return QStringLiteral("No fault");
    case 'A': return QStringLiteral("No tuner match");
    case 'C': return QStringLiteral("High drain current");
    case 'D': return QStringLiteral("High dissipation");
    case 'H': return QStringLiteral("High supply voltage");
    case 'I': return QStringLiteral("High input power");
    case 'L': return QStringLiteral("Low supply voltage");
    case 'P': return QStringLiteral("High output power");
    case 'R': return QStringLiteral("High reflected power");
    case 'S': return QStringLiteral("High SWR");
    case 'T': return QStringLiteral("High temperature");
    default: return QStringLiteral("Unknown fault (%1)").arg(quint8(code));
    }
}
QString faultUnit(char code)
{
    switch (code) {
    case 'N': return QStringLiteral("power-ons");
    case 'A': case 'S': return QStringLiteral(":1");
    case 'C': return QStringLiteral("A");
    case 'D': case 'P': case 'R': case 'I': return QStringLiteral("W");
    case 'H': case 'L': return QStringLiteral("V");
    case 'T': return QStringLiteral("°C");
    default: return QStringLiteral("raw");
    }
}
std::optional<MemoryBin> decodeMemoryBin(const QByteArray& frame)
{
    static const QRegularExpression pattern(QStringLiteral(
        "^\\^DM F([0-9]{5}) BN([0-9]{2}) BIN([0-9]{3}) FR([0-9]{5})-([0-9]{5})\\s+"
        "AN1 (EMPTY|BY[BN] SI[TA] CR[0-9A-F]{2} LR[0-9A-F]{2})\\s+"
        "AN2 (EMPTY|BY[BN] SI[TA] CR[0-9A-F]{2} LR[0-9A-F]{2})$"));
    const auto match = pattern.match(QString::fromLatin1(frame).simplified());
    if (!match.hasMatch()) { return {}; }
    MemoryBin result;
    result.frequencyKhz = match.captured(1).toInt(); result.band = match.captured(2).toInt();
    result.index = match.captured(3).toInt(); result.firstKhz = match.captured(4).toInt(); result.lastKhz = match.captured(5).toInt();
    if (result.band > 10 || result.firstKhz < 1500 || result.lastKhz > 60000 || result.firstKhz > result.lastKhz ||
        result.frequencyKhz < result.firstKhz || result.frequencyKhz > result.lastKhz) { return {}; }
    for (int antenna = 1; antenna <= 2; ++antenna) {
        MemoryMatch entry; entry.antenna = antenna;
        const QString text = match.captured(5 + antenna);
        entry.empty = text == QStringLiteral("EMPTY");
        if (!entry.empty) {
            const QStringList fields = text.split(' ');
            entry.bypassed = fields[0].endsWith('B');
            entry.side = fields[1].endsWith('T') ? CapacitorSide::Transmitter : CapacitorSide::Antenna;
            entry.capacitorMask = fields[2].mid(2).toInt(nullptr, 16); entry.inductorMask = fields[3].mid(2).toInt(nullptr, 16);
        }
        result.matches.append(entry);
    }
    return result;
}
}
