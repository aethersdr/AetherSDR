#pragma once

#include <optional>

#include <QMetaType>
#include <QString>
#include <QStringList>

namespace AetherSDR {

// Normalized, vendor-neutral transmit-status delta (aetherd RFC 2.3 —
// TransmitModel touchpoint). Same typed, compiler-checked, present-only contract
// as SliceDelta: a backend populates only the fields the wire reported, and
// TransmitModel::applyChanges applies exactly those. Covers the five Flex
// transmit-family status planes (core transmit, interlock, ATU, APD, APD
// sampler) — the FlexBackend decode owns the SmartSDR wire translation (key
// names, "1"→bool, ok-guarded + clamped numeric parses, uppercase, list split).
//
// The ATU status is carried as its raw wire token (`atuStatusRaw`); the model
// owns the ATUStatus enum + parse. The APD sampler is per-TX-antenna: a single
// delta carries one antenna's (txAnt, available, selected) triple.
struct TransmitDelta {
    // ── Core transmit ──
    std::optional<int>     rfPower;          // clamped 0..100
    std::optional<int>     tunePower;        // clamped 0..100
    std::optional<bool>    tune;
    std::optional<bool>    mox;
    std::optional<double>  transmitFreq;     // MHz
    std::optional<int>     maxPowerLevel;    // unclamped
    std::optional<QString> tuneMode;
    std::optional<QString> txSliceMode;
    std::optional<bool>    showTxInWaterfall;
    std::optional<bool>    metInRx;

    // ── Mic / monitor / processor ──
    std::optional<QString> micSelection;     // uppercased
    std::optional<int>     micLevel;         // 0..100
    std::optional<bool>    micAcc;
    std::optional<bool>    speechProcEnable;
    std::optional<int>     speechProcLevel;  // 0..100
    // compander / dexp are aliased on the wire: `compander` (or `dexp` when
    // compander is absent) drives BOTH the compander (mic) and dexp (phone)
    // member pairs; likewise compander_level / noise_gate_level.
    std::optional<bool>    compander;
    std::optional<int>     companderLevel;   // 0..100
    std::optional<bool>    dax;
    std::optional<bool>    sbMonitor;
    std::optional<int>     monGainSb;        // 0..100

    // ── VOX / phone ──
    std::optional<bool>    voxEnable;
    std::optional<int>     voxLevel;         // 0..100
    std::optional<int>     voxDelay;         // 0..100
    std::optional<bool>    micBoost;
    std::optional<bool>    micBias;
    std::optional<bool>    syncCwx;
    std::optional<int>     amCarrierLevel;   // 0..100
    std::optional<int>     txFilterLow;      // 0..10000 Hz
    std::optional<int>     txFilterHigh;     // 0..10000 Hz

    // ── CW ──
    std::optional<int>     cwSpeed;          // 5..100
    std::optional<int>     cwPitch;          // 100..6000
    std::optional<bool>    cwBreakIn;
    std::optional<int>     cwDelay;          // 0..2000
    std::optional<bool>    cwSidetone;
    std::optional<bool>    cwIambic;
    std::optional<int>     cwIambicMode;     // 0..1
    std::optional<bool>    cwSwapPaddles;
    std::optional<bool>    cwlEnabled;
    std::optional<int>     monGainCw;        // 0..100
    std::optional<int>     monPanCw;         // 0..100

    // ── Interlock (unclamped ints; no model emit) ──
    std::optional<int>     accTxDelay;
    std::optional<int>     tx1Delay;
    std::optional<int>     tx2Delay;
    std::optional<int>     tx3Delay;
    std::optional<int>     txDelay;
    std::optional<int>     interlockTimeout;
    std::optional<int>     accTxReqPolarity;
    std::optional<int>     rcaTxReqPolarity;

    // ── ATU ──
    std::optional<QString> atuStatusRaw;     // model parses to ATUStatus
    std::optional<bool>    atuEnabled;
    std::optional<bool>    memoriesEnabled;
    std::optional<bool>    usingMemory;

    // ── APD (amplifier protection) ──
    std::optional<bool>    apdEnabled;
    std::optional<bool>    apdConfigurable;
    std::optional<bool>    apdEqActive;
    // Bare `equalizer_reset` flag: model clears apdEqActive + emits the reset
    // signal. Engaged (regardless of value) iff the wire carried the flag.
    std::optional<bool>    apdEqualizerReset;

    // ── APD sampler (per-TX-antenna; one antenna per delta) ──
    std::optional<QString>     apdSamplerTxAnt;      // the map key (uppercased)
    std::optional<QStringList> apdSamplerAvailable;  // INTERNAL-prepended, uppercased
    std::optional<QString>     apdSamplerSelected;   // uppercased (model applies fallback)
};

}  // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::TransmitDelta)
