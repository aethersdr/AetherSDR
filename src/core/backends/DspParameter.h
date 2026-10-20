#pragma once

#include "core/backends/DspParameterReset.h"

#include <QList>
#include <QMap>
#include <QMetaType>
#include <QStringList>
#include <array>

namespace AetherSDR {

enum class DspParameter {
    NrDelay, NrWindow, NrIsdft, NrAdapt,
    AnfDelay, AnfWindow, AnfIsdft, AnfAdapt,
    NrlDelay, NrlTaps, NrlLeakage,
    AnflDelay, AnflTaps, AnflLeakage,
    AnftMinDb, AnftToneDiff, NrfWindow
};

struct DspParameterSpec {
    DspParameter parameter;
    DspParameterResetTarget algorithm;
    const char* id;
    const char* label;
    int minimum;
    int maximum;
    bool powerOfTwo;
    QStringList choices;
};

inline const std::array<DspParameterSpec, 17>& dspParameterSpecs()
{
    using Parameter = DspParameter;
    using Algorithm = DspParameterResetTarget;
    // Enum values are canonical choice indices, never legacy firmware codes.
    static const std::array<DspParameterSpec, 17> specs{{
        {Parameter::NrDelay, Algorithm::Nr, "nrDelay", "Delay", 1, 512, false, {}},
        {Parameter::NrWindow, Algorithm::Nr, "nrWindow", "Window", 32, 512, true, {}},
        {Parameter::NrIsdft, Algorithm::Nr, "nrIsdft", "Inverse SDFT", 0, 2, false,
            {QStringLiteral("one"), QStringLiteral("half"), QStringLiteral("zero")}},
        {Parameter::NrAdapt, Algorithm::Nr, "nrAdapt", "Adaptation", 0, 2, false,
            {QStringLiteral("auto"), QStringLiteral("fast"), QStringLiteral("slow")}},
        {Parameter::AnfDelay, Algorithm::Anf, "anfDelay", "Delay", 1, 512, false, {}},
        {Parameter::AnfWindow, Algorithm::Anf, "anfWindow", "Window", 32, 512, true, {}},
        {Parameter::AnfIsdft, Algorithm::Anf, "anfIsdft", "Inverse SDFT", 0, 2, false,
            {QStringLiteral("one"), QStringLiteral("half"), QStringLiteral("zero")}},
        {Parameter::AnfAdapt, Algorithm::Anf, "anfAdapt", "Adaptation", 0, 2, false,
            {QStringLiteral("auto"), QStringLiteral("fast"), QStringLiteral("slow")}},
        {Parameter::NrlDelay, Algorithm::Nrl, "nrlDelay", "Delay", 1, 512, false, {}},
        {Parameter::NrlTaps, Algorithm::Nrl, "nrlTaps", "Taps", 1, 512, false, {}},
        {Parameter::NrlLeakage, Algorithm::Nrl, "nrlLeakage", "Leakage", 0, 100, false, {}},
        {Parameter::AnflDelay, Algorithm::Anfl, "anflDelay", "Delay", 1, 512, false, {}},
        {Parameter::AnflTaps, Algorithm::Anfl, "anflTaps", "Taps", 1, 512, false, {}},
        {Parameter::AnflLeakage, Algorithm::Anfl, "anflLeakage", "Leakage", 0, 100, false, {}},
        // FlexRadio's published DSP Controls Panel reference defines these dB bounds.
        {Parameter::AnftMinDb, Algorithm::Anft, "anftMinDb", "Tone min (dB)", -150, -50, false, {}},
        {Parameter::AnftToneDiff, Algorithm::Anft, "anftToneDiff", "Tone diff (dB)", 1, 60, false, {}},
        {Parameter::NrfWindow, Algorithm::Nrf, "nrfWindow", "Window", 512, 8192, true, {}}
    }};
    return specs;
}

inline const DspParameterSpec* dspParameterSpec(DspParameter parameter)
{
    for (const DspParameterSpec& spec : dspParameterSpecs()) {
        if (spec.parameter == parameter) {
            return &spec;
        }
    }
    return nullptr;
}

inline bool validDspParameterValue(DspParameter parameter, int value)
{
    const DspParameterSpec* spec = dspParameterSpec(parameter);
    return spec && value >= spec->minimum && value <= spec->maximum
        && (!spec->powerOfTwo || (value > 0 && (value & (value - 1)) == 0));
}

struct DspParameterRequest {
    DspParameter parameter{DspParameter::NrDelay};
    int value{0};
    bool valid() const { return validDspParameterValue(parameter, value); }
};

// Missing entries are unobserved, not an instruction to restore local defaults.
using DspParameterValues = QMap<DspParameter, int>;

struct DspParameterControl {
    QList<DspParameter> parameters;
    QStringList modes;
    QStringList notchModes;
    DspParameterResetControl::AdvancedAccess advancedAccess{
        DspParameterResetControl::AdvancedAccess::Unknown};

    bool supports(DspParameter parameter) const
    {
        return dspParameterSpec(parameter) && parameters.contains(parameter);
    }
    bool permits(DspParameter parameter) const
    {
        return supports(parameter)
            && advancedAccess == DspParameterResetControl::AdvancedAccess::Granted;
    }
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::DspParameter)
Q_DECLARE_METATYPE(AetherSDR::DspParameterRequest)
Q_DECLARE_METATYPE(AetherSDR::DspParameterValues)
