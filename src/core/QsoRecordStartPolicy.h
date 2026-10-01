#pragma once

// QsoRecordStartPolicy — may a QSO recording start? (#4629)
//
// Client-side recording captures RadioModel::rxDemodAudioReady, which on a Flex
// is fed by the `remote_audio_rx` stream. That stream exists only when PC Audio
// is enabled (scheduleRxAudioStreamEnsure), deliberately (#1071), so with PC
// Audio off a client-side recording would write an empty WAV with no error.
// Refuse it and say why instead.
//
// Radio-side recording (`slice set <n> record=1`) does not involve the client
// audio path and must never be blocked here; `Allow` for it is load-bearing.
//
// `backendOwnsRxAudio` is a required input. A backend that demodulates
// in-process feeds the recorder over the seam and never uses `remote_audio_rx`,
// so PC Audio is irrelevant to it. MainWindow's PC Audio lock does not cover
// this: it is gated on hostModulates && canTransmit, which Sim fails
// (ownsRxAudio=true, PC Audio can be turned off). The caller supplies the value
// live through a provider callback, so it cannot go stale across a backend swap.
//
// Pure and header-only so every case is unit-testable without a radio, event
// loop or settings store.

namespace AetherSDR {

enum class RecordStartDecision {
    Allow,
    // Client-Side mode with PC Audio disabled: the RX audio stream the recorder
    // depends on does not exist. Starting would produce a header-only WAV.
    BlockedPcAudioDisabled,
    // The operator selected RADIO-SIDE recording, so the radio is the recorder
    // and QsoRecorder — which only ever writes a LOCAL file — has no business
    // running. See the note below on why this is a separate decision.
    BlockedRecordingModeIsRadio,
};

// The question this answers is "may THE CLIENT RECORDER start?", not "may a
// recording start?". The distinction matters: in Radio-Side mode a recording
// absolutely may start, just not this one — `slice set <n> record=1` goes to
// the radio and never reaches QsoRecorder.
//
// An earlier revision answered the broader question and returned Allow for
// Radio-Side, reasoning that radio-side recording must never be blocked. That
// guarantee is real and still holds, but it belongs to the ROUTING layer, not
// here: MainWindow_Wiring and the MIDI binding both test RecordingMode and send
// radio-side straight to SliceModel, so this policy is never consulted on the
// operator's radio-side path at all. Returning Allow instead let a caller that
// does NOT route — AutomationServer::doRecord — open a local WAV in Radio-Side
// mode and leave a spurious header-only file behind. Caught in review of the
// PR that introduced this file.
//
// Why its own value rather than folding into BlockedPcAudioDisabled: the reason
// is not PC Audio, and reporting it as such would put a dialog about PC Audio
// in front of an operator whose problem is entirely different. A wrong
// explanation is worse than none.
//
// `clientSideMode`      AppSettings "RecordingMode" == "Client" (the default).
// `pcAudioEnabled`      AppSettings "PcAudioEnabled" == "True" (the default).
// `backendOwnsRxAudio`  IRadioBackend::ownsRxAudio() — the connected backend
//                       demodulates in-process and feeds the recorder over the
//                       seam, so `remote_audio_rx` (and PC Audio) is not in the
//                       path at all. False for Flex, true for HL2 and the sim.
constexpr RecordStartDecision evaluateRecordStart(bool clientSideMode,
                                                  bool pcAudioEnabled,
                                                  bool backendOwnsRxAudio)
{
    // Wrong recorder for the operator's chosen mode — the radio is recording,
    // or should be. Checked first: it is a routing error, not an audio one, and
    // the audio questions below are meaningless in this mode.
    if (!clientSideMode)
        return RecordStartDecision::BlockedRecordingModeIsRadio;

    // Seam-native audio bypasses the PC Audio gate entirely.
    if (backendOwnsRxAudio)
        return RecordStartDecision::Allow;

    if (!pcAudioEnabled)
        return RecordStartDecision::BlockedPcAudioDisabled;

    return RecordStartDecision::Allow;
}

} // namespace AetherSDR
