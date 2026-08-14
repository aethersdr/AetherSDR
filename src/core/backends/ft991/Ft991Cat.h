#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

namespace AetherSDR::ft991 {

// Stateless codec for the Yaesu FT-991/FT-991A CAT wire (the CAT Operation
// Reference Manual is the authority for every frame here). Build functions
// return a complete ";"-terminated frame; parse() takes ONE frame with the
// terminator already stripped. Keeping this free of QSerialPort makes the
// wire vocabulary testable without hardware and keeps Ft991Device about
// transport and scheduling only.
//
// The mirror-image of SmartCatProtocol (our TS-2000-dialect CAT *server*):
// same textual family, opposite direction — but the FT-991's mode table is
// Yaesu's own, NOT the TS-2000 one, so the two must not share maps.
namespace Ft991Cat {

// The FT-991's answer to "ID;". The FT-991A answers the same.
inline const char* kRadioId = "0670";

// ---- frame builders (all validated; empty on out-of-range input) ----
QByteArray setFrequency(double hz);          // "FA<9 digits>;"
QByteArray queryFrequency();                 // "FA;"
QByteArray setMode(const QString& neutral);  // "MD0<x>;"
QByteArray queryMode();                      // "MD0;"
QByteArray setPtt(bool tx);                  // "TX1;" / "TX0;"
QByteArray queryPtt();                       // "TX;"
QByteArray querySMeter();                    // "SM0;"
QByteArray setPowerWatts(int watts);         // "PC<3 digits>;" (5..100)
QByteArray queryPower();                     // "PC;"
QByteArray setAgc(const QString& neutral);   // "GT0<x>;" off/fast/med/slow
QByteArray queryAgc();                       // "GT0;"
QByteArray queryId();                        // "ID;"
QByteArray setAutoInformation(bool on);      // "AI1;" / "AI0;"

// ---- radio-side DSP (widths per the CAT manual's SH tables; hamlib's
// ft991 driver encodes the same numbers and served as the cross-check) ----

// Which SH width table the mode uses. Fixed modes (AM/FM) have no SH —
// only the NA narrow/wide pair.
enum class WidthFamily { Ssb, CwData, Fixed };
WidthFamily widthFamilyForMode(const QString& neutral);

// Snap a requested audio width (Hz) to the nearest SH index for this mode.
// Returns -1 for Fixed modes (no SH table).
int nearestWidthIndex(const QString& neutral, int widthHz);
// The width (Hz) an SH index means for this mode; index 0 is the radio's
// per-mode default and depends on the NA flag. -1 when unknown/Fixed.
int widthForIndex(const QString& neutral, int index, bool narrow);
// The NA (narrow) flag the radio wants for this width in this mode.
bool narrowForWidth(const QString& neutral, int widthHz);

QByteArray setWidthIndex(int index);         // "SH0<2 digits>;"
QByteArray queryWidth();                     // "SH0;"
QByteArray setNarrow(bool narrow);           // "NA0<x>;"
QByteArray queryNarrow();                    // "NA0;"
QByteArray setNoiseBlanker(bool on);         // "NB0<x>;"
QByteArray queryNoiseBlanker();              // "NB0;"
QByteArray setNoiseBlankerLevel(int level0to10);   // "NL00<2 digits>;"
QByteArray queryNoiseBlankerLevel();         // "NL0;"
QByteArray setAutoNotch(bool on);            // "BC0<x>;" — the DNF
QByteArray queryAutoNotch();                 // "BC0;"
QByteArray setNoiseReduction(bool on);       // "NR0<x>;" — the DNR
QByteArray queryNoiseReduction();            // "NR0;"
QByteArray setNoiseReductionLevel(int level1to15);   // "RL0<2 digits>;"
QByteArray queryNoiseReductionLevel();       // "RL0;"
QByteArray setManualNotch(bool on);          // "BP00001;" / "BP00000;"
QByteArray setManualNotchHz(int hz);         // "BP01<3 digits>;" (10 Hz steps)
QByteArray queryManualNotch();               // "BP00;"
QByteArray queryManualNotchHz();             // "BP01;"
QByteArray queryTxPowerMeter();              // "RM5;" — PO, 0..255
QByteArray queryTxSwrMeter();                // "RM6;" — SWR, 0..255

// ---- clarifier (RIT/XIT) ----
//
// ONE offset, TWO enable flags: the FT-991 has a single clarifier knob
// shared by RX and TX, so RIT and XIT cannot hold different values here.
// The absolute-offset setter is therefore "clear, then step": RC zeroes
// the shared offset and RU/RD walk it to the target in one command each.
QByteArray setRitEnabled(bool on);           // "RT0;" / "RT1;"
QByteArray setXitEnabled(bool on);           // "XT0;" / "XT1;"
QByteArray clearClarifier();                 // "RC;"
QByteArray setClarifierOffset(int hz);       // "RC;" + "RU/RD<4 digits>;"
QByteArray queryInfo();                      // "IF;" — freq+clarifier+flags

// The IF response carries the clarifier state; parse() reports it as an
// Info response with these fields populated.

// The FT-991 tuning range this backend will command, Hz (RX coverage; the
// radio itself refuses TX outside its bands, which is its call, not ours).
inline constexpr double kTuningMinHz = 30'000.0;
inline constexpr double kTuningMaxHz = 470'000'000.0;

// ---- mode mapping ----
// Neutral slice vocabulary <-> the FT-991 MD0 code. catToMode is LOSSY on
// purpose (RTTY-L and DATA-L both display as DIGL); the raw code is only
// re-sent on an explicit operator mode change, so a lossy display never
// silently rewrites the radio's actual mode.
QString catToMode(QChar code);               // '1'..'E' -> "LSB".."FM"; empty if unknown
QChar modeToCat(const QString& neutral);     // "USB" -> '2'; '\0' if unmappable

// True when `neutral` demodulates the LOWER sideband — the pan-mapping
// direction (audio ascending maps RF DESCENDING from the dial).
bool isLowSideband(const QString& neutral);

// ---- response parsing ----
struct Response {
    enum class Kind {
        Frequency,   // frequencyHz valid
        Mode,        // mode (neutral) valid
        Tx,          // txState valid: 0 = RX, 1 = CAT TX, 2 = radio PTT/other
        SMeter,      // raw valid: 0..255
        Power,       // raw valid: watts 5..100
        Agc,         // agcMode (neutral) valid
        Id,          // id valid ("0670" expected)
        Width,           // raw valid: SH index
        Narrow,          // raw valid: 0/1
        NoiseBlanker,    // raw valid: 0/1
        NoiseBlankerLevel,   // raw valid: 0..10
        AutoNotch,       // raw valid: 0/1 (DNF)
        NoiseReduction,      // raw valid: 0/1 (DNR)
        NoiseReductionLevel, // raw valid: 1..15
        ManualNotch,     // raw valid: 0/1
        ManualNotchFreq, // raw valid: Hz
        TxPowerMeter,    // raw valid: 0..255 (PO)
        TxSwrMeter,      // raw valid: 0..255
        Info,            // frequencyHz, mode, clarifierHz, ritOn, xitOn valid
        Rejected,    // the radio answered "?" — command refused/unparsed
        Unknown,     // well-formed but not a frame this backend consumes
    };
    Kind kind = Kind::Unknown;
    double frequencyHz = 0.0;
    QString mode;
    QString agcMode;
    int txState = 0;
    int raw = 0;
    QString id;
    int clarifierHz = 0;   // signed; Info only
    bool ritOn = false;    // Info only
    bool xitOn = false;    // Info only
};

// Parse one ';'-stripped frame. nullopt only for an empty frame.
std::optional<Response> parse(const QByteArray& frame);

}  // namespace Ft991Cat

}  // namespace AetherSDR::ft991
