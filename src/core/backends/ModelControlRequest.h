#pragma once

#include <QMetaType>
#include <QString>
#include <cmath>
#include <variant>

namespace AetherSDR {

// Desktop requests, not observations or daemon permissions. A field identifies
// the one register being edited; companion values are for paired host controls.
struct IncrementalTuning { bool enabled; int hz; };
struct DtcsSetting { int code; bool txReverse; bool rxReverse; };
struct RepeaterSetting { QString direction; double offsetHz; QString toneMode; double toneHz; };
struct ProcessorSetting { bool enabled; int level; };
struct VoxSetting { bool enabled; int level; int delay; };
struct MonitorSetting { bool enabled; int level; };
struct TxPassband { int lowHz; int highHz; };
struct ApdSamplerSetting { QString antenna; QString port; };
enum class TunePowerContext { Deferred, LiveLocalCarrier, UnqualifiedCarrier };

struct SliceControlRequest {
    enum class Field {
        TxAntenna, Rit, Xit, DaxChannel, RttyMark, RttyShift, DiglOffset,
        DiguOffset, TxSlice, ActiveSlice, Record, Play, FmToneMode, FmToneValue,
        FmRxToneValue, FmDtcs, RepeaterDirection, RepeaterOffset, TxOffset,
        FmDeviation, RfGain, Diversity, EscEnabled, EscGain, EscPhase, RepeaterRecall,
        Count
    };
    using Value = std::variant<bool, int, double, QString, IncrementalTuning,
                               DtcsSetting, RepeaterSetting>;
    Field field{Field::Count};
    Value value;
    enum class Origin { Operator, RadioDefaultRestore };
    Origin origin{Origin::Operator};
    bool valid() const;
    QString label() const;
};

struct TransmitControlRequest {
    enum class Field {
        RfPower, TunePower, TuneMode, MicInput, MicGain, MicAccessory,
        ProcessorEnabled, ProcessorLevel, Dax, MonitorEnabled, MonitorLevel,
        VoxEnabled, VoxLevel, VoxDelay, MicBoost, MicBias, AmCarrier,
        ExpanderEnabled, ExpanderLevel, Filter, CwSpeed, CwPitch, CwBreakIn,
        CwDelay, CwSidetone, CwIambic, CwIambicMode, CwSwap, CwlEnabled,
        CwMonitorGain, CwMonitorPan, TxProfile, MicProfile, ApdEnabled,
        ApdSampler, ApdReset, AtuMemories, AtuClear, Count
    };
    using Value = std::variant<bool, int, QString, ProcessorSetting, VoxSetting,
                               MonitorSetting, TxPassband, ApdSamplerSetting>;
    Field field{Field::Count};
    Value value;
    bool valid() const;
    QString label() const;
};

// Text is a value, never a fragment of command grammar. Vendor-specific token
// and quoted-string restrictions are additionally checked by that encoder.
inline bool controlTextValid(const QString& text)
{
    if (text.isEmpty() || text.size() > 256) { return false; }
    for (const QChar ch : text) {
        if (!ch.isPrint() || ch == QLatin1Char('"')) { return false; }
    }
    return true;
}

inline bool SliceControlRequest::valid() const
{
    if (origin != Origin::Operator
        && !(origin == Origin::RadioDefaultRestore && field == Field::RttyMark)) {
        return false;
    }
    const int* integer = std::get_if<int>(&value);
    const double* number = std::get_if<double>(&value);
    const QString* text = std::get_if<QString>(&value);
    switch (field) {
    case Field::Rit: case Field::Xit: {
        const auto* tuning = std::get_if<IncrementalTuning>(&value);
        return tuning && tuning->hz >= -99999 && tuning->hz <= 99999;
    }
    case Field::DaxChannel: return integer && *integer >= 0 && *integer <= 8;
    case Field::RttyMark: case Field::RttyShift: case Field::DiglOffset:
    case Field::DiguOffset: case Field::FmDeviation:
        return integer != nullptr; // full signed register domain; no invented vendor limit
    case Field::RfGain: return integer != nullptr;
    case Field::TxSlice: case Field::ActiveSlice: case Field::Record: case Field::Play:
    case Field::Diversity: case Field::EscEnabled:
        return std::holds_alternative<bool>(value);
    case Field::TxAntenna: case Field::FmToneMode: case Field::RepeaterDirection:
        return text && controlTextValid(*text);
    case Field::FmToneValue: case Field::FmRxToneValue:
        return number && std::isfinite(*number) && *number >= 0 && *number <= 1000;
    case Field::RepeaterOffset: case Field::TxOffset:
        return number && std::isfinite(*number);
    case Field::EscGain:
        return number && std::isfinite(*number) && *number >= 0 && *number <= 2;
    case Field::EscPhase:
        return number && std::isfinite(*number);
    case Field::FmDtcs: {
        const auto* dtcs = std::get_if<DtcsSetting>(&value);
        return dtcs && dtcs->code >= 0 && dtcs->code <= 777;
    }
    case Field::RepeaterRecall: {
        const auto* recall = std::get_if<RepeaterSetting>(&value);
        return recall && controlTextValid(recall->direction) && controlTextValid(recall->toneMode)
            && std::isfinite(recall->offsetHz) && std::abs(recall->offsetHz) <= 1.0e9
            && std::isfinite(recall->toneHz) && recall->toneHz >= 0 && recall->toneHz <= 1000;
    }
    case Field::Count: return false;
    }
    return false;
}

inline bool TransmitControlRequest::valid() const
{
    const int* integer = std::get_if<int>(&value);
    const QString* text = std::get_if<QString>(&value);
    switch (field) {
    case Field::RfPower: case Field::TunePower: case Field::MicGain:
    case Field::AmCarrier: case Field::ExpanderLevel:
    case Field::CwMonitorGain: case Field::CwMonitorPan:
        return integer && *integer >= 0 && *integer <= 100;
    case Field::CwSpeed: return integer && *integer >= 5 && *integer <= 100;
    case Field::CwPitch: return integer && *integer >= 100 && *integer <= 6000;
    case Field::CwDelay: return integer && *integer >= 0 && *integer <= 2000;
    case Field::CwIambicMode: return integer && *integer >= 0 && *integer <= 1;
    case Field::TuneMode: case Field::MicInput: case Field::TxProfile: case Field::MicProfile:
        return text && controlTextValid(*text);
    case Field::ProcessorEnabled: case Field::ProcessorLevel: {
        const auto* setting = std::get_if<ProcessorSetting>(&value);
        return setting && setting->level >= 0 && setting->level <= 100;
    }
    case Field::VoxEnabled: case Field::VoxLevel: case Field::VoxDelay: {
        const auto* setting = std::get_if<VoxSetting>(&value);
        return setting && setting->level >= 0 && setting->level <= 100
            && setting->delay >= 0 && setting->delay <= 100;
    }
    case Field::MonitorEnabled: case Field::MonitorLevel: {
        const auto* setting = std::get_if<MonitorSetting>(&value);
        return setting && setting->level >= 0 && setting->level <= 100;
    }
    case Field::Filter: {
        const auto* filter = std::get_if<TxPassband>(&value);
        return filter && filter->lowHz >= 0 && filter->highHz <= 10000
            && filter->highHz > filter->lowHz;
    }
    case Field::ApdSampler: {
        const auto* sampler = std::get_if<ApdSamplerSetting>(&value);
        return sampler && controlTextValid(sampler->antenna) && controlTextValid(sampler->port);
    }
    case Field::MicAccessory: case Field::Dax: case Field::MicBoost: case Field::MicBias:
    case Field::ExpanderEnabled: case Field::CwBreakIn: case Field::CwSidetone:
    case Field::CwIambic: case Field::CwSwap: case Field::CwlEnabled:
    case Field::ApdEnabled: case Field::ApdReset: case Field::AtuMemories: case Field::AtuClear:
        return std::holds_alternative<bool>(value);
    case Field::Count: return false;
    }
    return false;
}

inline QString SliceControlRequest::label() const
{
    switch (field) {
    case Field::TxAntenna: return QStringLiteral("tx antenna");
    case Field::Rit: return QStringLiteral("rit");
    case Field::Xit: return QStringLiteral("xit");
    case Field::DaxChannel: return QStringLiteral("dax channel");
    case Field::RttyMark: return QStringLiteral("rtty mark");
    case Field::RttyShift: return QStringLiteral("rtty shift");
    case Field::DiglOffset: return QStringLiteral("digl offset");
    case Field::DiguOffset: return QStringLiteral("digu offset");
    case Field::TxSlice: return QStringLiteral("tx slice");
    case Field::ActiveSlice: return QStringLiteral("active slice");
    case Field::Record: return QStringLiteral("record");
    case Field::Play: return QStringLiteral("play");
    case Field::FmToneMode: return QStringLiteral("fm tone mode");
    case Field::FmToneValue: return QStringLiteral("fm tone value");
    case Field::FmRxToneValue: return QStringLiteral("fm rx tone value");
    case Field::FmDtcs: return QStringLiteral("fm dtcs");
    case Field::RepeaterDirection: return QStringLiteral("repeater direction");
    case Field::RepeaterOffset: return QStringLiteral("repeater offset");
    case Field::TxOffset: return QStringLiteral("tx offset");
    case Field::FmDeviation: return QStringLiteral("fm deviation");
    case Field::RfGain: return QStringLiteral("rf gain");
    case Field::Diversity: return QStringLiteral("diversity");
    case Field::EscEnabled: return QStringLiteral("esc enabled");
    case Field::EscGain: return QStringLiteral("esc gain");
    case Field::EscPhase: return QStringLiteral("esc phase");
    case Field::RepeaterRecall: return QStringLiteral("repeater recall");
    case Field::Count: break;
    }
    return QStringLiteral("invalid control");
}

inline QString TransmitControlRequest::label() const
{
    switch (field) {
    case Field::RfPower: return QStringLiteral("rf power");
    case Field::TunePower: return QStringLiteral("tune power");
    case Field::TuneMode: return QStringLiteral("tune mode");
    case Field::MicInput: return QStringLiteral("mic input");
    case Field::MicGain: return QStringLiteral("mic gain");
    case Field::MicAccessory: return QStringLiteral("mic accessory");
    case Field::ProcessorEnabled: return QStringLiteral("processor enabled");
    case Field::ProcessorLevel: return QStringLiteral("processor level");
    case Field::Dax: return QStringLiteral("dax");
    case Field::MonitorEnabled: return QStringLiteral("monitor enabled");
    case Field::MonitorLevel: return QStringLiteral("monitor level");
    case Field::VoxEnabled: return QStringLiteral("vox enabled");
    case Field::VoxLevel: return QStringLiteral("vox level");
    case Field::VoxDelay: return QStringLiteral("vox delay");
    case Field::MicBoost: return QStringLiteral("mic boost");
    case Field::MicBias: return QStringLiteral("mic bias");
    case Field::AmCarrier: return QStringLiteral("am carrier");
    case Field::ExpanderEnabled: return QStringLiteral("expander enabled");
    case Field::ExpanderLevel: return QStringLiteral("expander level");
    case Field::Filter: return QStringLiteral("filter");
    case Field::CwSpeed: return QStringLiteral("cw speed");
    case Field::CwPitch: return QStringLiteral("cw pitch");
    case Field::CwBreakIn: return QStringLiteral("cw break in");
    case Field::CwDelay: return QStringLiteral("cw delay");
    case Field::CwSidetone: return QStringLiteral("cw sidetone");
    case Field::CwIambic: return QStringLiteral("cw iambic");
    case Field::CwIambicMode: return QStringLiteral("cw iambic mode");
    case Field::CwSwap: return QStringLiteral("cw swap");
    case Field::CwlEnabled: return QStringLiteral("cwl enabled");
    case Field::CwMonitorGain: return QStringLiteral("cw monitor gain");
    case Field::CwMonitorPan: return QStringLiteral("cw monitor pan");
    case Field::TxProfile: return QStringLiteral("tx profile");
    case Field::MicProfile: return QStringLiteral("mic profile");
    case Field::ApdEnabled: return QStringLiteral("apd enabled");
    case Field::ApdSampler: return QStringLiteral("apd sampler");
    case Field::ApdReset: return QStringLiteral("apd reset");
    case Field::AtuMemories: return QStringLiteral("atu memories");
    case Field::AtuClear: return QStringLiteral("atu clear");
    case Field::Count: break;
    }
    return QStringLiteral("invalid control");
}

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::SliceControlRequest)
Q_DECLARE_METATYPE(AetherSDR::TransmitControlRequest)
