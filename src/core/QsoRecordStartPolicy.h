#pragma once

// QsoRecordStartPolicy — may a QSO recording start? (#4629)
//
// Client-Side recording captures RadioModel::rxDemodAudioReady, which on a Flex
// is fed by PanadapterStream::audioDataReady — the `remote_audio_rx` VITA-49
// stream. That stream is created ONLY when PC Audio is enabled
// (RadioModel::scheduleRxAudioStreamEnsure, which returns early on
// PcAudioEnabled=False and removes an already-owned stream). With PC Audio off
// there is no stream, so feedRxAudio() is never called, and the recorder
// happily opened a file and wrote a 44-byte WAV header to it, then waited
// forever for samples that could not arrive. The operator got a correctly named
// file containing nothing, and no error — because from the recorder's point of
// view nothing had gone wrong. It received silence-of-absence rather than
// silence-of-signal, and could not tell the two apart.
//
// Gating stream creation on PC Audio is deliberate, not an oversight: #1071
// removed TCI's auto-create for exactly this reason ("auto-creating the stream
// overrode the user's explicit PC Audio toggle"). So the fix is not to
// circumvent the gate but to refuse the recording and say why.
//
// RADIO-SIDE RECORDING DOES NOT DEPEND ON PC AUDIO and must never be blocked
// here. It is `slice set <n> record=1` (SliceModel::setRecordOn) — the radio
// records to its own storage and the client's audio path is not involved at
// all. It stays available as the answer for an operator who monitors on the
// radio's own speaker, so `Allow` for radio-side is a load-bearing case, not a
// fallthrough.
//
// Pure and header-only so every case is unit-testable without a radio, an
// event loop, or a settings store — mirroring DaxRestorePolicy /
// KiwiSdrTxMutePolicy / SliceRecreatePolicy.
//
// ── Why `backendOwnsRxAudio` is a REQUIRED input, not a refinement ───────────
//
// A backend that demodulates in-process feeds the recorder over the seam
// (RadioModel::wireRxDemodAudioBus binds backendAudioFrameReady when
// IRadioBackend::ownsRxAudio() is true) and never touches `remote_audio_rx` at
// all. PC Audio is irrelevant to it, and blocking it would break a recording
// that works.
//
// It is tempting to argue this input is unnecessary because MainWindow
// force-enables AND LOCKS PC Audio on such a backend. That argument is WRONG,
// and the counter-example ships today: the lock is gated on
// `caps.hostModulates && caps.canTransmit` (MainWindow_Session.cpp), and
//
//   HL2   ownsRxAudio=true,  hostModulates=true,  canTransmit=true  → locked
//   Sim   ownsRxAudio=true,  hostModulates=FALSE, canTransmit=FALSE → NOT locked
//
// SimBackend is the only backend that owns its RX audio without locking PC
// Audio on — the operator can freely turn PC Audio off in demo mode, and the
// sim's audio still reaches the recorder. Two inputs would refuse that
// recording for a reason that does not apply to it.
//
// The caller supplies this live (QsoRecorder holds a provider callback rather
// than a cached bool) so there is no flag to go stale across a backend swap —
// a stale flag failing open is the shape of the bug this whole file exists to
// fix.

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
