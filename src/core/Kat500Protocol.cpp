#include "Kat500Protocol.h"
#include <QRegularExpression>
#include <QStringList>
#include <array>
#include <cmath>

namespace AetherSDR::Kat500 {
namespace {
std::optional<int> integer(const QByteArray& text, int low, int high, int base = 10)
{
    const QByteArray value = text.trimmed();
    if (value.isEmpty() || value.size() > 8) { return {}; }
    for (const char c : value) {
        if (!((c >= '0' && c <= '9') || (base == 16 && c >= 'A' && c <= 'F'))) { return {}; }
    }
    bool ok = false;
    const int result = value.toInt(&ok, base);
    if (!ok || result < low || result > high) { return {}; }
    return result;
}
std::optional<double> decimal(const QByteArray& text)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9]{1,2}(\\.[0-9]{1,2})?$"));
    const QString value = QString::fromLatin1(text.trimmed());
    if (!pattern.match(value).hasMatch()) { return {}; }
    const double result = value.toDouble();
    if (result > 99.99) { return {}; }
    return result;
}
QByteArray bandCode(int band) { return QByteArray::number(band).rightJustified(2, '0'); }
bool validKey(SettingKey key)
{
    const bool perBand = key.setting >= Setting::EnabledAntenna && key.setting <= Setting::KeySwr;
    return perBand ? key.band >= 0 && key.band <= 10 &&
        (key.setting == Setting::EnabledAntenna ? key.antenna >= 1 && key.antenna <= 3 : key.antenna == 0)
        : key.band == -1 && key.antenna == 0;
}
QByteArray configPrefix(SettingKey key)
{
    if (!validKey(key)) { return {}; }
    switch (key.setting) {
    case Setting::AmpKeyPower: return "AKIP";
    case Setting::PowerOnDc: return "PSI";
    case Setting::SleepIdle: return "SL";
    case Setting::RetuneDistance: return "FDT";
    case Setting::EnabledAntenna: return "AE" + bandCode(key.band) + QByteArray::number(key.antenna);
    case Setting::PreferredAntenna: return "AP" + bandCode(key.band);
    case Setting::SlotsPerBin: return "AB" + bandCode(key.band);
    case Setting::AutoFineTune: return "AFT" + bandCode(key.band);
    case Setting::AutoSwr: return "ST" + bandCode(key.band) + 'A';
    case Setting::BypassSwr: return "ST" + bandCode(key.band) + 'B';
    case Setting::KeySwr: return "ST" + bandCode(key.band) + 'K';
    }
    return {};
}
bool validValue(Setting setting, double value)
{
    if (!std::isfinite(value)) { return false; }
    if (setting == Setting::AutoSwr || setting == Setting::BypassSwr || setting == Setting::KeySwr) {
        return value >= (setting == Setting::AutoSwr ? 1.5 : 1.0) && value <= 99.99;
    }
    if (std::floor(value) != value) { return false; }
    switch (setting) {
    case Setting::AmpKeyPower: return value >= 0 && value <= 1500;
    case Setting::RetuneDistance: return value >= 0 && value <= 65535;
    case Setting::SlotsPerBin: return value >= 1 && value <= 6;
    case Setting::PreferredAntenna: return value >= 0 && value <= 3;
    default: return value == 0 || value == 1;
    }
}
}
void FrameParser::feed(const QByteArray& bytes)
{
    for (const char c : bytes) {
        if (c == ';') {
            if (!m_discard && m_callback) { m_callback(m_buffer.trimmed()); }
            m_buffer.clear(); m_discard = false;
        } else if (c == '\r' || c == '\n') {
            continue;
        } else if (c < 32 || c > 126 || m_buffer.size() >= 192) {
            m_buffer.clear(); m_discard = true;
        } else if (!m_discard) {
            m_buffer.append(c);
        }
    }
}
bool applyStatus(const QByteArray& frame, Status& status)
{
    if (frame == "KAT500" || frame == "kat500") { status.identity = QString::fromLatin1(frame); return true; }
    if (frame.startsWith("RV")) {
        static const QRegularExpression version(QStringLiteral("^RV[0-9]{2}\\.[0-9]{2}$"));
        if (!version.match(QString::fromLatin1(frame)).hasMatch()) { return false; }
        status.firmware = QString::fromLatin1(frame.mid(2)); return true;
    }
    if (frame.startsWith("SN")) {
        const auto value = integer(frame.mid(2), 0, 99999);
        if (!value) { return false; }
        status.serial = QString::number(*value).rightJustified(5, QLatin1Char('0')); return true;
    }
    const std::array<std::pair<const char*, std::optional<bool>*>, 4> booleans{{
        {"PS", &status.powerOn}, {"TP", &status.tuning}, {"AMPI", &status.ampKeyInterrupted}, {"ATTN", &status.attenuator}}};
    for (const auto& [prefix, destination] : booleans) {
        const QByteArray key(prefix);
        if (frame == key + '0' || frame == key + '1') { *destination = frame.endsWith('1'); return true; }
    }
    if (frame == "MDA" || frame == "MDM" || frame == "MDB") {
        status.mode = frame.endsWith('A') ? Mode::Auto : frame.endsWith('M') ? Mode::Manual : Mode::Bypass; return true;
    }
    if (frame == "BYPB" || frame == "BYPN") { status.bypassed = frame.endsWith('B'); return true; }
    if (frame == "SIDET" || frame == "SIDEA") {
        status.side = frame.endsWith('T') ? CapacitorSide::Transmitter : CapacitorSide::Antenna; return true;
    }
    struct Number { const char* prefix; int low; int high; int base; std::optional<int>* destination; };
    const std::array<Number, 11> numbers{{
        {"VFWD",0,4095,10,&status.forwardAdc}, {"VRFL",0,4095,10,&status.reflectedAdc},
        {"FLT",0,4,10,&status.faultCode}, {"AN",1,3,10,&status.antenna},
        {"BN",0,10,10,&status.band}, {"FC",0,99999,10,&status.countedFrequencyKhz},
        {"FX",0,99999,10,&status.catFrequencyKhz}, {"F",0,99999,10,&status.frequencyKhz},
        {"C",0,255,16,&status.capacitorMask}, {"L",0,255,16,&status.inductorMask},
        {"",0,0,10,nullptr}}};
    if (frame.startsWith("FY ")) {
        static const QRegularExpression bin(QStringLiteral("^FY [0-9]{1,5}-[0-9]{1,5}$"));
        if (!bin.match(QString::fromLatin1(frame)).hasMatch()) { return false; }
        status.frequencyBin = QString::fromLatin1(frame.mid(3)); return true;
    }
    for (const Number& number : numbers) {
        if (!number.destination) { continue; }
        const QByteArray prefix(number.prefix);
        if (frame.startsWith(prefix)) {
            const auto value = integer(frame.mid(prefix.size()), number.low, number.high, number.base);
            if (value) { *number.destination = *value; return true; }
        }
    }
    for (const bool bypass : {true, false}) {
        const QByteArray prefix = bypass ? "VSWRB" : "VSWR";
        if (frame.startsWith(prefix)) {
            const auto value = decimal(frame.mid(prefix.size()));
            if (value) { (bypass ? status.bypassSwr : status.swr) = *value; return true; }
        }
    }
    return false;
}
QList<SettingKey> configurationKeys()
{
    QList<SettingKey> keys{{Setting::AmpKeyPower}, {Setting::PowerOnDc}, {Setting::SleepIdle}, {Setting::RetuneDistance}};
    for (int band = 0; band <= 10; ++band) {
        for (int antenna = 1; antenna <= 3; ++antenna) { keys.append({Setting::EnabledAntenna, band, antenna}); }
        for (Setting setting : {Setting::PreferredAntenna, Setting::SlotsPerBin, Setting::AutoFineTune,
                                Setting::KeySwr, Setting::AutoSwr, Setting::BypassSwr}) { keys.append({setting, band}); }
    }
    return keys;
}
QByteArray configurationQuery(SettingKey key)
{
    const QByteArray prefix = configPrefix(key);
    return prefix.isEmpty() ? QByteArray{} : prefix + ';';
}
QByteArray configurationSet(SettingKey key, double value)
{
    const QByteArray prefix = configPrefix(key);
    if (prefix.isEmpty() || !validValue(key.setting, value)) { return {}; }
    const bool swr = key.setting >= Setting::AutoSwr && key.setting <= Setting::KeySwr;
    const bool space = key.setting == Setting::AmpKeyPower || key.setting == Setting::RetuneDistance;
    return prefix + (space ? " " : "") + QByteArray::number(value, 'f', swr ? 2 : 0) + ';';
}
std::optional<double> configurationValue(SettingKey key, const QByteArray& frame)
{
    const QByteArray prefix = configPrefix(key);
    if (prefix.isEmpty() || !frame.startsWith(prefix)) { return {}; }
    QByteArray tail = frame.mid(prefix.size()).trimmed();
    if (key.setting == Setting::AmpKeyPower) {
        static const QRegularExpression power(QStringLiteral("^([0-9]{1,4})W VFWD ([0-9]{1,4})$"));
        const auto match = power.match(QString::fromLatin1(tail));
        if (!match.hasMatch() || match.captured(2).toInt() > 4095) { return {}; }
        tail = match.captured(1).toLatin1();
    }
    const bool swr = key.setting >= Setting::AutoSwr && key.setting <= Setting::KeySwr;
    std::optional<double> value;
    if (swr) { value = decimal(tail); }
    else if (const auto number = integer(tail, 0, 65535)) { value = *number; }
    if (!value || !validValue(key.setting, *value)) { return {}; }
    return value;
}
QString validateConfiguration(const Configuration& confirmed, const Configuration& edits)
{
    Configuration result = confirmed;
    for (auto it = edits.cbegin(); it != edits.cend(); ++it) {
        if (!confirmed.contains(it.key())) { return QStringLiteral("Read configuration before editing."); }
        if (configurationSet(it.key(), it.value()).isEmpty()) { return QStringLiteral("Invalid configuration value."); }
        result[it.key()] = it.value();
    }
    for (int band = 0; band < 11; ++band) {
        int enabled = 0;
        for (int antenna = 1; antenna <= 3; ++antenna) {
            if (result.value({Setting::EnabledAntenna,band,antenna}, -1) == 1) { ++enabled; }
        }
        const int preferred = static_cast<int>(result.value({Setting::PreferredAntenna,band}, -1));
        if (!enabled || preferred < 0 || (preferred && result.value({Setting::EnabledAntenna,band,preferred}, 0) != 1)) {
            return QStringLiteral("Every band needs an enabled antenna and an enabled preferred antenna (or Last used).");
        }
    }
    return {};
}
QByteArray buildFrequency(int khz) { return isSupportedFrequency(khz) ? "F " + QByteArray::number(khz) + ';' : QByteArray{}; }
QByteArray buildErase(int band, int antenna)
{
    return band >= 0 && band <= 10 && antenna >= 0 && antenna <= 3
        ? "EM" + bandCode(band) + QByteArray::number(antenna) + ';' : QByteArray{};
}
bool matchesStatusQuery(const QByteArray& query, const QByteArray& frame)
{
    if (query == "I;") { return frame == "KAT500" || frame == "kat500"; }
    if (query == ";") { return frame.isEmpty(); }
    const QByteArray prefix = query.chopped(1);
    if (!frame.startsWith(prefix)) { return false; }
    // A longer command must not satisfy a shorter one (PSI/PS, VSWRB/VSWR).
    if (frame.size() > prefix.size() && frame[prefix.size()] >= 'A' && frame[prefix.size()] <= 'Z'
        && prefix != "MD" && prefix != "BYP" && prefix != "SIDE" && prefix != "C" && prefix != "L") { return false; }
    Status scratch;
    return applyStatus(frame, scratch);
}
QString bandName(int band)
{
    static const QStringList names{QStringLiteral("160m"),QStringLiteral("80m"),QStringLiteral("60m"),QStringLiteral("40m"),
        QStringLiteral("30m"),QStringLiteral("20m"),QStringLiteral("17m"),QStringLiteral("15m"),QStringLiteral("12m"),QStringLiteral("10m"),QStringLiteral("6m")};
    return band >= 0 && band < names.size() ? names[band] : QStringLiteral("Unknown band");
}
QString settingName(Setting setting)
{
    static const QStringList names{QStringLiteral("Amplifier key interrupt power"),QStringLiteral("Power on with DC"),
        QStringLiteral("Sleep when idle"),QStringLiteral("Enabled antenna"),QStringLiteral("Preferred antenna"),
        QStringLiteral("ATU settings per bin"),QStringLiteral("Auto fine tune"),QStringLiteral("Auto tune SWR"),
        QStringLiteral("Bypass SWR"),QStringLiteral("Amplifier key interrupt SWR"),QStringLiteral("Frequency retune distance")};
    const int index = static_cast<int>(setting);
    return index >= 0 && index < names.size() ? names[index] : QStringLiteral("Unknown setting");
}
int relayTotal(bool capacitor, int mask)
{
    constexpr std::array<int,8> capacitors{8,22,39,82,180,330,680,1360};
    constexpr std::array<int,8> inductors{50,110,230,480,1000,2100,4400,9000};
    if (mask < 0 || mask > 255) { return 0; }
    int total = 0;
    for (int bit = 0; bit < 8; ++bit) { if (mask & (1 << bit)) { total += (capacitor ? capacitors : inductors)[bit]; } }
    return total;
}
int relayMaskForValue(bool capacitor, int value)
{
    int best = 0;
    for (int mask = 0; mask < 256; ++mask) {
        if (relayTotal(capacitor, mask) <= value && relayTotal(capacitor, mask) > relayTotal(capacitor, best)) { best = mask; }
    }
    return best;
}
QString currentFaultName(int code)
{
    static const QStringList names{QStringLiteral("No current fault"),QStringLiteral("No suitable tuning match"),
        QStringLiteral("Power exceeds limit for SWR"),QStringLiteral("Power exceeds relay switching limit"),QStringLiteral("Amplifier key interrupt SWR exceeded")};
    return code >= 0 && code < names.size() ? names[code] : QStringLiteral("Unknown fault %1").arg(code);
}
std::optional<MemoryBin> decodeMemoryBin(const QList<QByteArray>& frames)
{
    if (frames.size() < 2 || frames.size() > 40) { return {}; }
    static const QRegularExpression header(QStringLiteral("^DM ([0-9]{1,5})-([0-9]{1,5})$"));
    const auto range = header.match(QString::fromLatin1(frames.first()));
    if (!range.hasMatch()) { return {}; }
    MemoryBin bin; bin.firstKhz = range.captured(1).toInt(); bin.lastKhz = range.captured(2).toInt();
    if (bin.firstKhz > bin.lastKhz) { return {}; }
    std::optional<MemoryMatch> match;
    for (int i = 1; i < frames.size(); ++i) {
        const QByteArray& frame = frames[i];
        if (frame.endsWith(" UNUSED")) {
            const auto unused = integer(frame.chopped(7), 0, 6);
            if (!unused || match || i != frames.size() - 1 || bin.matches.size() + *unused != 6) { return {}; }
            bin.unused = *unused; return bin;
        }
        if (frame.startsWith("AN")) {
            const auto antenna = integer(frame.mid(2),1,3);
            if (!antenna || match) { return {}; }
            match = MemoryMatch{}; match->antenna = *antenna;
        } else if (!match) { return {}; }
        else if (frame == "BYP") {
            if (match->bypassed || match->side || match->capacitorMask || match->inductorMask) { return {}; }
            match->bypassed = true;
        }
        else if (frame.startsWith("VSWRB")) {
            const auto swr = decimal(frame.mid(5));
            if (!swr || (!match->bypassed && (!match->side || !match->capacitorMask || !match->inductorMask))) { return {}; }
            match->bypassSwr = *swr; bin.matches.append(*match); match.reset();
        } else {
            Status status;
            if (!applyStatus(frame, status)) { return {}; }
            if (match->bypassed) { return {}; }
            if (status.side && !match->side) { match->side = status.side; }
            else if (status.capacitorMask && !match->capacitorMask) { match->capacitorMask = status.capacitorMask; }
            else if (status.inductorMask && !match->inductorMask) { match->inductorMask = status.inductorMask; }
            else { return {}; }
        }
    }
    return {};
}
} // namespace AetherSDR::Kat500
