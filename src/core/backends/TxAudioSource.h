#pragma once

#include <QMetaType>

namespace AetherSDR {

// WHERE A BLOCK OF TRANSMIT AUDIO ORIGINATED, which decides whose level it is.
// IRadioBackend::submitTxAudio() carries it; Hl2TxDsp::processAudioBlock() is
// the only thing in the tree that acts on it.
//
//   Microphone       the operator set this level and is standing there to hear
//                    the result — the capture chain, and the AX.25 modem, whose
//                    AFSK amplitude is a fixed constant the operator has no
//                    other way to move (AetherAx25LibmodemShim.cpp,
//                    kTxAfskAmplitude). The mic slider applies.
//   ClientLeveled    external TCI/DAX client audio. The sender already applied
//                    its own power control — WSJT-X's Pwr slider attenuates the
//                    audio it streams — so a host-modulating backend must not
//                    add makeup over it (#4796). The mic slider applies as the
//                    proportional attenuator #4796 left it.
//   EngineGenerated  audio this engine produced already shaped and already
//                    levelled, with NOBODY WATCHING: the WSPR pump, whose frame
//                    keys for 111.6 s unattended. The mic slider DOES NOT
//                    apply — a microphone control has no business moving an
//                    unattended beacon.
//
// The tag is a claim about ORIGIN, not about treatment: what a backend does
// with it is the backend's business, and a radio that modulates on its own side
// does nothing with it at all.
//
// WHY IT IS THREE STATES. It replaced a bool, `clientLeveled`, that could only
// ask "did an external client set this level?" — true for TCI/DAX, false for
// everything else — so the operator's microphone and the engine's own beacon
// shared one bucket. That was harmless while Hl2TxDsp's ALC carried up to 40 dB
// of makeup and normalised every source onto its target. Removing the makeup
// (#5646) made the difference load-bearing.
//
// This type has its own header rather than living in IRadioBackend.h because
// AudioEngine.h and Hl2TxDsp.h both name it, and neither has any other reason
// to parse 1300 lines of backend seam.
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
