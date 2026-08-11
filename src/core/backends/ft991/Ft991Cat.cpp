#include "core/backends/ft991/Ft991Cat.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>

namespace AetherSDR::ft991 {

namespace Ft991Cat {

namespace {

// The FT-991 MD0 mode table (CAT manual, MD command). Kept as one table so
// the two directions cannot drift apart.
struct ModeRow {
    char code;
    const char* neutral;   // what SliceModel displays
    bool lowSideband;
    bool setTarget;        // the row modeToCat picks for this neutral name
};
constexpr ModeRow kModes[] = {
    {'1', "LSB",  true,  true},
    {'2', "USB",  false, true},
    {'3', "CW",   false, true},    // CW-U
    {'4', "FM",   false, true},
    {'5', "AM",   false, true},
    {'6', "DIGL", true,  false},   // RTTY-LSB, displayed as DIGL
    {'7', "CWL",  true,  true},    // CW-R
    {'8', "DIGL", true,  true},    // DATA-LSB
    {'9', "DIGU", false, false},   // RTTY-USB, displayed as DIGU
    {'A', "FM",   false, false},   // DATA-FM
    {'B', "NFM",  false, true},    // FM-N
    {'C', "DIGU", false, true},    // DATA-USB
    {'D', "AM",   false, false},   // AM-N
    {'E', "FM",   false, false},   // C4FM
};

// Neutral names with no FT-991 mode of their own, folded onto the nearest
// real mode rather than refused — a mode verb that silently does nothing is
// the worse failure.
QString foldNeutral(const QString& u)
{
    if (u == QLatin1String("CWU")) return QStringLiteral("CW");
    if (u == QLatin1String("DSB") || u == QLatin1String("SAM")
        || u == QLatin1String("DRM"))
        return QStringLiteral("AM");
    if (u == QLatin1String("WBFM") || u == QLatin1String("WFM")
        || u == QLatin1String("NFM"))
        return u == QLatin1String("NFM") ? u : QStringLiteral("FM");
    return u;
}

// The FT-991 SH width tables (CAT manual "SH" — shared with the FT-891;
// hamlib ft991_ssb_widths/ft991_cw_widths carry the same numbers). Index 0
// is the radio's per-mode DEFAULT width, whose meaning depends on NA.
constexpr int kSsbWidths[] = {
    0, 200, 400, 600, 850, 1100, 1350, 1500, 1650, 1800, 1950, 2100,
    2200, 2300, 2400, 2500, 2600, 2700, 2800, 2900, 3000, 3200,
};
constexpr int kCwDataWidths[] = {
    0, 50, 100, 150, 200, 250, 300, 350, 400, 450, 500, 800, 1200,
    1400, 1700, 2000, 2400, 3000,
};
// NA is narrow at or below these (per table; the CAT manual's split point).
constexpr int kSsbNarrowMaxHz = 1800;
constexpr int kCwDataNarrowMaxHz = 500;

}  // namespace

WidthFamily widthFamilyForMode(const QString& neutral)
{
    const QString u = foldNeutral(neutral.trimmed().toUpper());
    if (u == QLatin1String("USB") || u == QLatin1String("LSB"))
        return WidthFamily::Ssb;
    if (u == QLatin1String("CW") || u == QLatin1String("CWL")
        || u == QLatin1String("DIGU") || u == QLatin1String("DIGL"))
        return WidthFamily::CwData;
    return WidthFamily::Fixed;
}

int nearestWidthIndex(const QString& neutral, int widthHz)
{
    const WidthFamily family = widthFamilyForMode(neutral);
    if (family == WidthFamily::Fixed)
        return -1;
    const int* table = family == WidthFamily::Ssb ? kSsbWidths : kCwDataWidths;
    const int count = family == WidthFamily::Ssb
        ? static_cast<int>(std::size(kSsbWidths))
        : static_cast<int>(std::size(kCwDataWidths));
    int best = 1;
    int bestDistance = std::numeric_limits<int>::max();
    for (int i = 1; i < count; ++i) {   // index 0 is "default", never chosen
        const int distance = std::abs(table[i] - widthHz);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

int widthForIndex(const QString& neutral, int index, bool narrow)
{
    const WidthFamily family = widthFamilyForMode(neutral);
    if (family == WidthFamily::Fixed)
        return -1;
    const bool ssb = family == WidthFamily::Ssb;
    const int* table = ssb ? kSsbWidths : kCwDataWidths;
    const int count = ssb ? static_cast<int>(std::size(kSsbWidths))
                          : static_cast<int>(std::size(kCwDataWidths));
    if (index > 0 && index < count)
        return table[index];
    if (index != 0)
        return -1;
    // Index 0: the per-mode default (CAT manual; hamlib mirrors it) — SSB
    // narrow 1500 / wide 2400; CW narrow 500 / wide 2400; DATA narrow 300 /
    // wide 500.
    const QString u = foldNeutral(neutral.trimmed().toUpper());
    if (ssb)
        return narrow ? 1500 : 2400;
    if (u == QLatin1String("CW") || u == QLatin1String("CWL"))
        return narrow ? 500 : 2400;
    return narrow ? 300 : 500;
}

bool narrowForWidth(const QString& neutral, int widthHz)
{
    switch (widthFamilyForMode(neutral)) {
    case WidthFamily::Ssb:    return widthHz <= kSsbNarrowMaxHz;
    case WidthFamily::CwData: return widthHz <= kCwDataNarrowMaxHz;
    case WidthFamily::Fixed:  return false;   // AM/FM narrow is a mode choice
    }
    return false;
}

QByteArray setWidthIndex(int index)
{
    if (index < 0 || index > 99)
        return {};
    return QByteArrayLiteral("SH0")
        + QByteArray::number(index).rightJustified(2, '0') + ';';
}

QByteArray queryWidth()  { return QByteArrayLiteral("SH0;"); }
QByteArray setNarrow(bool narrow)
{
    return narrow ? QByteArrayLiteral("NA01;") : QByteArrayLiteral("NA00;");
}
QByteArray queryNarrow() { return QByteArrayLiteral("NA0;"); }
QByteArray setNoiseBlanker(bool on)
{
    return on ? QByteArrayLiteral("NB01;") : QByteArrayLiteral("NB00;");
}
QByteArray queryNoiseBlanker() { return QByteArrayLiteral("NB0;"); }

QByteArray setNoiseBlankerLevel(int level0to10)
{
    const int level = std::clamp(level0to10, 0, 10);
    return QByteArrayLiteral("NL00")
        + QByteArray::number(level).rightJustified(2, '0') + ';';
}

QByteArray queryNoiseBlankerLevel() { return QByteArrayLiteral("NL0;"); }
QByteArray setAutoNotch(bool on)
{
    return on ? QByteArrayLiteral("BC01;") : QByteArrayLiteral("BC00;");
}
QByteArray queryAutoNotch() { return QByteArrayLiteral("BC0;"); }
QByteArray setNoiseReduction(bool on)
{
    return on ? QByteArrayLiteral("NR01;") : QByteArrayLiteral("NR00;");
}
QByteArray queryNoiseReduction() { return QByteArrayLiteral("NR0;"); }

QByteArray setNoiseReductionLevel(int level1to15)
{
    const int level = std::clamp(level1to15, 1, 15);
    return QByteArrayLiteral("RL0")
        + QByteArray::number(level).rightJustified(2, '0') + ';';
}

QByteArray queryNoiseReductionLevel() { return QByteArrayLiteral("RL0;"); }
QByteArray setManualNotch(bool on)
{
    return on ? QByteArrayLiteral("BP00001;") : QByteArrayLiteral("BP00000;");
}

QByteArray setManualNotchHz(int hz)
{
    // BP01 carries the notch position in 10 Hz steps, 10..3200 Hz.
    const int steps = std::clamp(hz / 10, 1, 320);
    return QByteArrayLiteral("BP01")
        + QByteArray::number(steps).rightJustified(3, '0') + ';';
}

QByteArray queryManualNotch()   { return QByteArrayLiteral("BP00;"); }
QByteArray queryManualNotchHz() { return QByteArrayLiteral("BP01;"); }
QByteArray queryTxPowerMeter()  { return QByteArrayLiteral("RM5;"); }
QByteArray queryTxSwrMeter()    { return QByteArrayLiteral("RM6;"); }

QByteArray setRitEnabled(bool on)
{
    return on ? QByteArrayLiteral("RT1;") : QByteArrayLiteral("RT0;");
}

QByteArray setXitEnabled(bool on)
{
    return on ? QByteArrayLiteral("XT1;") : QByteArrayLiteral("XT0;");
}

QByteArray clearClarifier() { return QByteArrayLiteral("RC;"); }

QByteArray setClarifierOffset(int hz)
{
    // The wire has no absolute-offset command: RC zeroes the shared
    // clarifier and RU/RD step it. Both frames in one write so nothing can
    // observe the intermediate zero.
    const int clamped = std::clamp(hz, -9999, 9999);
    QByteArray out = clearClarifier();
    if (clamped == 0)
        return out;
    out += (clamped > 0 ? QByteArrayLiteral("RU") : QByteArrayLiteral("RD"));
    out += QByteArray::number(std::abs(clamped)).rightJustified(4, '0');
    out += ';';
    return out;
}

QByteArray queryInfo() { return QByteArrayLiteral("IF;"); }

QByteArray setFrequency(double hz)
{
    if (!(hz >= kTuningMinHz) || !(hz <= kTuningMaxHz))
        return {};
    const qint64 rounded = static_cast<qint64>(std::llround(hz));
    return QByteArrayLiteral("FA")
        + QByteArray::number(rounded).rightJustified(9, '0') + ';';
}

QByteArray queryFrequency() { return QByteArrayLiteral("FA;"); }

QByteArray setMode(const QString& neutral)
{
    const QChar code = modeToCat(neutral);
    if (code.isNull())
        return {};
    return QByteArrayLiteral("MD0") + char(code.toLatin1()) + ';';
}

QByteArray queryMode()   { return QByteArrayLiteral("MD0;"); }
QByteArray setPtt(bool tx) { return tx ? QByteArrayLiteral("TX1;") : QByteArrayLiteral("TX0;"); }
QByteArray queryPtt()    { return QByteArrayLiteral("TX;"); }
QByteArray querySMeter() { return QByteArrayLiteral("SM0;"); }

QByteArray setPowerWatts(int watts)
{
    const int w = std::clamp(watts, 5, 100);
    return QByteArrayLiteral("PC")
        + QByteArray::number(w).rightJustified(3, '0') + ';';
}

QByteArray queryPower() { return QByteArrayLiteral("PC;"); }

QByteArray setAgc(const QString& neutral)
{
    const QString m = neutral.trimmed().toLower();
    char code = '2';                          // med: the radio's MID
    if (m == QLatin1String("off"))  code = '0';
    else if (m == QLatin1String("fast")) code = '1';
    else if (m == QLatin1String("slow")) code = '3';
    return QByteArrayLiteral("GT0") + code + ';';
}

QByteArray queryAgc() { return QByteArrayLiteral("GT0;"); }
QByteArray queryId()  { return QByteArrayLiteral("ID;"); }
QByteArray setAutoInformation(bool on)
{
    return on ? QByteArrayLiteral("AI1;") : QByteArrayLiteral("AI0;");
}

QString catToMode(QChar code)
{
    const char c = code.toUpper().toLatin1();
    for (const ModeRow& row : kModes) {
        if (row.code == c)
            return QString::fromLatin1(row.neutral);
    }
    return {};
}

QChar modeToCat(const QString& neutral)
{
    const QString u = foldNeutral(neutral.trimmed().toUpper());
    for (const ModeRow& row : kModes) {
        if (row.setTarget && u == QLatin1String(row.neutral))
            return QLatin1Char(row.code);
    }
    return {};
}

bool isLowSideband(const QString& neutral)
{
    const QString u = foldNeutral(neutral.trimmed().toUpper());
    for (const ModeRow& row : kModes) {
        if (row.setTarget && u == QLatin1String(row.neutral))
            return row.lowSideband;
    }
    return false;
}

std::optional<Response> parse(const QByteArray& frame)
{
    if (frame.isEmpty())
        return std::nullopt;

    Response r;
    if (frame == "?") {
        r.kind = Response::Kind::Rejected;
        return r;
    }

    // Numeric tail helper: digits of `frame` from `from`, -1 on non-digits.
    const auto digits = [&frame](int from, int count) -> qint64 {
        if (frame.size() < from + count)
            return -1;
        qint64 v = 0;
        for (int i = from; i < from + count; ++i) {
            const char c = frame.at(i);
            if (c < '0' || c > '9')
                return -1;
            v = v * 10 + (c - '0');
        }
        return v;
    };

    if (frame.startsWith("FA")) {
        const qint64 hz = digits(2, frame.size() - 2);
        if (hz < 0)
            return r;   // Unknown — malformed digits must not tune anything
        r.kind = Response::Kind::Frequency;
        r.frequencyHz = static_cast<double>(hz);
        return r;
    }
    if (frame.startsWith("MD0") && frame.size() == 4) {
        const QString mode = catToMode(QLatin1Char(frame.at(3)));
        if (mode.isEmpty())
            return r;
        r.kind = Response::Kind::Mode;
        r.mode = mode;
        return r;
    }
    if (frame.startsWith("TX") && frame.size() == 3) {
        const qint64 s = digits(2, 1);
        if (s < 0)
            return r;
        r.kind = Response::Kind::Tx;
        r.txState = static_cast<int>(s);
        return r;
    }
    if (frame.startsWith("SM0")) {
        const qint64 v = digits(3, frame.size() - 3);
        if (v < 0 || v > 255)
            return r;
        r.kind = Response::Kind::SMeter;
        r.raw = static_cast<int>(v);
        return r;
    }
    if (frame.startsWith("PC")) {
        const qint64 v = digits(2, frame.size() - 2);
        if (v < 0 || v > 200)
            return r;
        r.kind = Response::Kind::Power;
        r.raw = static_cast<int>(v);
        return r;
    }
    if (frame.startsWith("GT0") && frame.size() == 4) {
        r.kind = Response::Kind::Agc;
        switch (frame.at(3)) {
        case '0': r.agcMode = QStringLiteral("off");  break;
        case '1': r.agcMode = QStringLiteral("fast"); break;
        case '3': r.agcMode = QStringLiteral("slow"); break;
        // '2' MID and '4'/'5'/'6' AUTO variants all display as med.
        default:  r.agcMode = QStringLiteral("med");  break;
        }
        return r;
    }
    if (frame.startsWith("ID")) {
        r.kind = Response::Kind::Id;
        r.id = QString::fromLatin1(frame.mid(2));
        return r;
    }
    if (frame.startsWith("SH0")) {
        const qint64 v = digits(3, frame.size() - 3);
        if (v < 0 || v > 99)
            return r;
        r.kind = Response::Kind::Width;
        r.raw = static_cast<int>(v);
        return r;
    }
    if (frame.startsWith("NA0") && frame.size() == 4) {
        const qint64 v = digits(3, 1);
        if (v < 0 || v > 1)
            return r;
        r.kind = Response::Kind::Narrow;
        r.raw = static_cast<int>(v);
        return r;
    }
    if (frame.startsWith("NB0") && frame.size() == 4) {
        const qint64 v = digits(3, 1);
        if (v < 0)
            return r;
        r.kind = Response::Kind::NoiseBlanker;
        r.raw = v != 0 ? 1 : 0;
        return r;
    }
    if (frame.startsWith("NL0")) {
        const qint64 v = digits(3, frame.size() - 3);
        if (v < 0 || v > 10)
            return r;
        r.kind = Response::Kind::NoiseBlankerLevel;
        r.raw = static_cast<int>(v);
        return r;
    }
    if (frame.startsWith("NR0") && frame.size() == 4) {
        const qint64 v = digits(3, 1);
        if (v < 0 || v > 1)
            return r;
        r.kind = Response::Kind::NoiseReduction;
        r.raw = static_cast<int>(v);
        return r;
    }
    if (frame.startsWith("RL0")) {
        const qint64 v = digits(3, frame.size() - 3);
        if (v < 0 || v > 15)
            return r;
        r.kind = Response::Kind::NoiseReductionLevel;
        r.raw = static_cast<int>(v);
        return r;
    }
    if (frame.startsWith("BC0") && frame.size() == 4) {
        const qint64 v = digits(3, 1);
        if (v < 0 || v > 1)
            return r;
        r.kind = Response::Kind::AutoNotch;
        r.raw = static_cast<int>(v);
        return r;
    }
    if (frame.startsWith("IF")) {
        // FT-991 IF response, ';' already stripped = 27 characters:
        //   [0..1] "IF"  [2..4] memory ch  [5..13] VFO-A frequency
        //   [14..18] clarifier (sign + 4 digits)  [19] RIT on  [20] XIT on
        //   [21] mode  [22] VFO/memory  [23] CTCSS  [24..25] "00"  [26] shift
        // The 9-digit frequency is what distinguishes it from the 26-char
        // FT-450-style response; anything else is not ours to interpret.
        if (frame.size() != 27)
            return r;
        const qint64 hz = digits(5, 9);
        const qint64 offset = digits(15, 4);   // [14] is the sign
        const char sign = frame.at(14);
        if (hz < 0 || offset < 0 || (sign != '+' && sign != '-'))
            return r;
        const QString mode = catToMode(QLatin1Char(frame.at(21)));
        if (mode.isEmpty())
            return r;
        r.kind = Response::Kind::Info;
        r.frequencyHz = static_cast<double>(hz);
        r.mode = mode;
        r.clarifierHz = static_cast<int>(sign == '-' ? -offset : offset);
        r.ritOn = frame.at(19) == '1';
        r.xitOn = frame.at(20) == '1';
        return r;
    }
    if (frame.startsWith("RM") && frame.size() == 6) {
        const qint64 v = digits(3, 3);
        if (v < 0 || v > 255)
            return r;
        if (frame.at(2) == '5')
            r.kind = Response::Kind::TxPowerMeter;
        else if (frame.at(2) == '6')
            r.kind = Response::Kind::TxSwrMeter;
        else
            return r;   // a meter this backend does not poll
        r.raw = static_cast<int>(v);
        return r;
    }
    // 7, not 8: parse() receives the frame with the ';' already stripped.
    if (frame.startsWith("BP0") && frame.size() == 7) {
        // "BP00nnn" = on/off, "BP01nnn" = position in 10 Hz steps.
        const qint64 v = digits(4, 3);
        if (v < 0)
            return r;
        if (frame.at(3) == '0') {
            r.kind = Response::Kind::ManualNotch;
            r.raw = v != 0 ? 1 : 0;
        } else if (frame.at(3) == '1') {
            r.kind = Response::Kind::ManualNotchFreq;
            r.raw = static_cast<int>(v) * 10;
        } else {
            return r;
        }
        return r;
    }
    return r;   // Unknown
}

}  // namespace Ft991Cat

}  // namespace AetherSDR::ft991
