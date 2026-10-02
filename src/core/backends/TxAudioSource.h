#pragma once

#include <QMetaType>

namespace AetherSDR {

// Where a block of transmit audio ORIGINATED, which decides whose level it is.
// Carried by IRadioBackend::submitTxAudio(); only Hl2TxDsp::processAudioBlock()
// acts on it.
//   Microphone       operator-levelled: the capture chain and the AX.25 modem
//                    (fixed kTxAfskAmplitude). The mic slider applies.
//   ClientLeveled    TCI/DAX client audio already power-controlled by its sender
//                    (WSJT-X Pwr); no makeup gain (#4796). The mic slider applies
//                    as a proportional attenuator.
//   EngineGenerated  shaped and levelled by this engine, unattended (the WSPR
//                    pump keys 111.6 s frames). The mic slider does NOT apply.
// Three states, not a bool, because the mic and the engine's beacon need
// different treatment since Hl2TxDsp lost its ALC makeup (#5646).
enum class TxAudioSource {
    Microphone = 0,
    ClientLeveled,
    EngineGenerated,
};

}  // namespace AetherSDR

// Declared as a metatype because it travels on AudioEngine's
// txFinalMonitorPcmReady signal, and AudioEngine lives on its own thread — a
// queued connection cannot marshal a type Qt has never been told about, and the
// failure is a runtime warning and a dropped signal, not a compile error.
Q_DECLARE_METATYPE(AetherSDR::TxAudioSource)
