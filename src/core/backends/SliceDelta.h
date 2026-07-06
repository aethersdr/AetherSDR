#pragma once

#include <optional>

#include <QMetaType>
#include <QString>
#include <QStringList>

namespace AetherSDR {

// Normalized, vendor-neutral slice-status delta (aetherd RFC 2.3 — SliceModel
// touchpoint). A backend populates only the fields the wire reported
// (std::optional engaged == "present"); SliceModel::applyChanges applies exactly
// those. This is the compiler-checked replacement for the prior stringly-keyed
// QVariantMap payload: a field-name typo between the backend decode and the model
// apply is now a compile error instead of a silently-dropped field.
//
// Value types are the canonical (vendor-neutral) types; the FlexBackend decode
// owns the SmartSDR wire→canonical translation (key names, "1"→bool, list split,
// lowercase, ok-guarded numeric parses). Kept out of any model header so the
// backend interface stays free of model dependencies (RFC layering).
struct SliceDelta {
    // Identity / tuning
    std::optional<QString>     panId;
    std::optional<QString>     letter;
    std::optional<double>      frequency;      // MHz
    std::optional<QString>     mode;
    std::optional<int>         filterLow;      // Hz
    std::optional<int>         filterHigh;     // Hz
    std::optional<QStringList> modeList;

    // Core state
    std::optional<bool>        active;
    std::optional<bool>        txSlice;
    std::optional<double>      rfGain;
    std::optional<double>      audioGain;
    std::optional<int>         audioPan;
    std::optional<bool>        audioMute;
    std::optional<bool>        inUse;
    std::optional<bool>        locked;
    std::optional<bool>        qsk;

    // Diversity
    std::optional<bool>        diversityChild;
    std::optional<bool>        diversityParent;
    std::optional<bool>        diversity;
    std::optional<int>         diversityIndex;

    // ESC (diversity beamforming)
    std::optional<bool>        esc;
    std::optional<double>      escGain;
    std::optional<double>      escPhaseShift;

    // Antennas
    std::optional<QStringList> rxAntennaList;
    std::optional<QStringList> txAntennaList;
    std::optional<QString>     rxAntenna;
    std::optional<QString>     txAntenna;

    // DSP toggles
    std::optional<bool>        nb;
    std::optional<bool>        nr;
    std::optional<bool>        anf;
    std::optional<bool>        nrl;
    std::optional<bool>        nrs;
    std::optional<bool>        rnn;
    std::optional<bool>        nrf;
    std::optional<bool>        anfl;
    std::optional<bool>        anft;
    std::optional<bool>        apf;
    // DSP levels
    std::optional<int>         apfLevel;
    std::optional<int>         nbLevel;
    std::optional<int>         nrLevel;
    std::optional<int>         anfLevel;
    std::optional<int>         nrlLevel;
    std::optional<int>         nrsLevel;
    std::optional<int>         nrfLevel;
    std::optional<int>         anflLevel;

    // AGC / squelch / RIT / XIT
    std::optional<QString>     agcMode;
    std::optional<int>         agcThreshold;
    std::optional<int>         agcOffLevel;
    std::optional<bool>        squelchOn;
    std::optional<int>         squelchLevel;
    std::optional<bool>        ritOn;
    std::optional<int>         ritFreq;
    std::optional<bool>        xitOn;
    std::optional<int>         xitFreq;

    // DAX / RTTY / DIG offsets
    std::optional<int>         daxChannel;
    std::optional<int>         rttyMark;
    std::optional<int>         rttyShift;
    std::optional<int>         diglOffset;
    std::optional<int>         diguOffset;

    // Record / playback (play is 3-state disabled/1/0 — carried raw, model interprets)
    std::optional<bool>        recordOn;
    std::optional<QString>     play;

    // FM duplex/repeater
    std::optional<QString>     fmToneMode;
    std::optional<double>      fmToneValue;
    std::optional<QString>     repeaterOffsetDir;
    std::optional<double>      fmRepeaterOffsetFreq;
    std::optional<double>      txOffsetFreq;
    std::optional<int>         fmDeviation;

    // Step (stepList carried raw — model builds the QVector<int>)
    std::optional<int>         step;
    std::optional<QString>     stepList;
};

}  // namespace AetherSDR

// Registered so the type can ride IRadioBackend::sliceChanged across a queued
// connection and be captured by QSignalSpy in tests. (Same-thread today resolves
// to a synchronous DirectConnection, but the registration keeps it correct if a
// backend is ever moved to a worker thread.)
Q_DECLARE_METATYPE(AetherSDR::SliceDelta)
