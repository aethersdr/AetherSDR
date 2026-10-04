#pragma once

// QsoRecordStartPolicy — may a QSO recording start? (#4629)
// Client-side recording taps rxDemodAudioReady, which on a Flex is fed by
// `remote_audio_rx`, which exists only with PC Audio enabled (#1071); without
// it the WAV would be silently empty, so refuse and say why. Backends that
// own RX audio (backendOwnsRxAudio) don't use that stream, so PC Audio is
// irrelevant to them; the caller supplies it live so it can't go stale.

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

// Answers "may the CLIENT recorder start?". Radio-side mode is refused here
// because radio-side recording is routed to SliceModel before this policy is
// consulted; a non-routing caller (AutomationServer::doRecord) must not open a
// local WAV. It gets its own value because the reason is not PC Audio.
//
// `clientSideMode`      Whether THIS CLIENT is the recorder: the operator's
//                       "RecordingMode" == "Client" (the default), or
//                       Radio-Side selected on a radio with no radio-side
//                       recorder to reach — see recordsOnClient() below.
// `pcAudioEnabled`      AppSettings "PcAudioEnabled" == "True" (the default).
// `backendOwnsRxAudio`  IRadioBackend::ownsRxAudio(): false for Flex, true for
//                       HL2 and the sim (remote_audio_rx not in the path).

// Which recorder does REC/PLAY reach? The one routing decision every surface
// shares (VFO flag, AetherRX, MIDI, this recorder's own start policy).
// Radio-Side recording is `slice set <n> record=/play=` on the command plane;
// a radio without one (HL2, ANAN, Icom, RTL) has no radio-side recorder, so
// there the setting falls back to the client recorder.
//
// `clientSideSetting`            AppSettings "RecordingMode" == "Client".
// `radioSideRecordingReachable`  RadioModel::radioSideRecordingReachable().
constexpr bool recordsOnClient(bool clientSideSetting,
                               bool radioSideRecordingReachable)
{
    return clientSideSetting || !radioSideRecordingReachable;
}

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
