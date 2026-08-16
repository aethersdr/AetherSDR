#pragma once

#include <QByteArray>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

#include "core/backends/AmpDelta.h"
#include "core/backends/GpsDelta.h"
#include "core/backends/MemoryDelta.h"
#include "core/backends/MeterDef.h"
#include "core/backends/NotchDelta.h"
#include "core/backends/ProfileDelta.h"
#include "core/backends/RadioCapabilities.h"
#include "core/backends/RestoredRadioState.h"
#include "core/backends/RadioDelta.h"
#include "core/backends/SliceDelta.h"
#include "core/backends/TransmitDelta.h"
#include "core/backends/TunerDelta.h"

namespace AetherSDR {

// Neutral, family-agnostic connect descriptor. Core fields cover the common
// case; vendor-specific parameters (SmartLink token, Kiwi endpoint path, …)
// ride in `params` so the interface never grows a per-vendor connect signature.
struct RadioConnectRequest {
    QString host;
    quint16 port = 0;
    QString serial;         // when a family identifies radios by serial
    QVariantMap params;     // family-specific extras (namespaced by the backend)
};

// The radio-facing seam of the engine (aetherd RFC §5.5). Everything that
// speaks a vendor wire protocol lives *behind* this interface, inside
// libaethercore; RadioModel and the (future) protocol see only this. The
// SmartSDR stack becomes the first implementor (FlexBackend, step 2.2) with
// ZERO behavior change; other radio families are added as further implementors
// without touching any client.
//
// Design decisions (RFC §5.5 open questions, resolved 2026-07-05):
//   Q2 — RadioModel keeps owning its sub-models (SliceModel, MeterModel, …);
//        the backend DRIVES them by emitting the signals below, which
//        RadioModel connects to. The backend does not own UI-facing model
//        objects. (Minimal churn; the models already communicate via signals.)
//   Q3 — ONE interface, not an RX-only/TX-capable type split. A receive-only
//        family reports capabilities().canTransmit == false and implements
//        setKeying() as a guarded no-op; the engine TX guard (RFC §6, above
//        this seam) denies keying when canTransmit is false.
//
// DSP location is invisible here (RFC §5.5): a backend whose hardware
// demodulates and computes FFTs is a thin protocol decoder; one that ships raw
// samples owns an engine-side DSP chain — either way it emits the same
// normalized signals, so no consumer can tell the difference.
//
// This is the CORE seed. It carries the lifecycle, capability, canonical-verb,
// and extension surface; it grows one method at a time as the touchpoint
// burndown (docs/architecture/aetherd-touchpoints.md) converts each gui→engine
// touchpoint into a protocol/backend verb. Do NOT dump all 140 touchpoints
// here at once.
class IRadioBackend : public QObject {
    Q_OBJECT

public:
    explicit IRadioBackend(QObject* parent = nullptr) : QObject(parent) {}
    ~IRadioBackend() override = default;

    // ---- identity & capability (feeds the protocol `welcome`, §4.1) ----
    virtual RadioCapabilities capabilities() const = 0;

    // True when this backend delivers demodulated RX audio over the seam
    // (audioFrameReady below) rather than through a Flex PanadapterStream.
    //
    // This is the gate the RX-audio wiring keys off, and it is deliberately a
    // question about THIS backend rather than a list of family names. Every
    // previous version of that decision was a `dynamic_cast<SimBackend*>` or an
    // `m_family != "flex"`, and each one had to be found and updated when a
    // backend was added — the double-feed buzz (#4490) is what happens when one
    // is missed: the sim's frames arrived over both routes and the engine
    // consumed at double rate, measured 48043 Hz against a nominal 24000.
    //
    // Note this is NOT "has no PanadapterStream". The sim has BOTH: a stream
    // carrying the old shim's synthetic scene, and real demodulated audio over
    // the seam. It answers true because the seam is the one that is real.
    virtual bool ownsRxAudio() const { return false; }

    // ---- connection lifecycle ----
    // Typed restore handoff (RFC #4603 proposal B): called by RadioModel
    // BEFORE connectRadio(), and only when this backend's declared
    // clientSettingsDomains is non-empty. The backend stashes what it wants
    // and applies it during connect/initial-push, validating its own
    // extension document at this boundary (Principle VII). Default no-op —
    // a radio-authoritative backend (Flex) never sees restored state.
    virtual void applyRestoredState(const RestoredRadioState& state)
    {
        Q_UNUSED(state);
    }
    virtual void connectRadio(const RadioConnectRequest& request) = 0;
    // The capture half of RadioStateMemory (RFC #4603 PR 3): a backend whose
    // declared clientSettingsDomains is non-empty reports its operating state
    // here on demand, and emits operatingStateChanged() (see signals) when it
    // moves. RadioModel debounces the signal and persists the snapshot via
    // RadioStateMemory::store — the backend never touches the settings store.
    virtual RestoredRadioState currentOperatingState() const { return {}; }
    virtual void disconnectRadio() = 0;
    virtual bool isConnected() const = 0;

    // ---- intents DOWN: canonical core-profile verbs (grow per burndown) ----
    // The backend translates each to its vendor wire protocol.
    virtual void setSliceFrequency(int sliceId, double hz) = 0;
    virtual void setSliceMode(int sliceId, const QString& mode) = 0;
    virtual void setSliceFilter(int sliceId, int lowHz, int highHz) = 0;
    // Receive AGC. mode is the neutral vocabulary the slice model uses —
    // "off" / "slow" / "med" / "fast"; thresholdDb is the operator's 0..100
    // AGC-threshold value. A backend whose hardware owns the AGC translates
    // both to its wire protocol; one that owns an engine-side DSP chain
    // configures that chain. Sent as a pair because a backend configuring a DSP
    // AGC generally needs both to make either meaningful.
    virtual void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) = 0;
    // Move the panadapter's centre — the receiver's WINDOW, not the slice.
    // A backend whose hardware streams a fixed window retunes it; one that
    // owns a DDC moves the NCO. Without this the UI can pan the view locally
    // while the data keeps arriving from the old window, and the waterfall
    // (which carries its own frequency extent) drifts off the display.
    //
    // WHY THE CENTRE CARRIES AN INTENT. On a radio whose scope window is slaved
    // to the operating frequency — every networked Icom in centre mode — moving
    // the window IS retuning, and the two callers of this verb want opposite
    // things from that. A DRAG is the operator asking to look somewhere else
    // and expecting the trace to follow the mouse. A ZOOM sends the centre only
    // because centre and bandwidth must travel together, and must not walk the
    // VFO across the band one click at a time. A backend that cannot tell them
    // apart has to choose which caller to break; every backend whose window is
    // genuinely independent of the VFO ignores this and treats both alike.
    enum class PanCenterIntent {
        Drag,   // the operator dragged the spectrum or waterfall
        Range,  // the centre rode along with a bandwidth/zoom change
    };
    // NO DEFAULT, deliberately. A default here is not merely redundant with the
    // pure-virtual-plus-override pair that catches implementations: it is a
    // hazard at CALL sites. A new caller that forgets the intent would silently
    // get Range, which on a backend whose scope window is slaved to the VFO
    // means the drag is refused and re-asserted — the precise bug this
    // parameter was added to fix, arriving with nothing to notice it by.
    // Making every caller state what it means is the whole value of the
    // parameter, and there is exactly one caller.
    virtual void setPanCenter(const QString& panId, double hz,
                              PanCenterIntent intent) = 0;

    // Change the panadapter's SPAN — how much spectrum the window covers.
    //
    // The sibling of setPanCenter, and it exists for the same reason. A backend
    // that owns its own DDC decides its span by choosing a decimation rate, and
    // nothing above this seam can do that for it. Without this verb the zoom
    // intent had nowhere to go: RadioModel wrote the requested span into
    // PanadapterModel and returned success, so the view widened while the
    // receiver kept delivering the old, narrower window. The VITA-49 tiles are
    // honest about their own extent, so the region the data never covered
    // rendered BLACK — the same lie #4142 fixed for pan center, reintroduced on
    // the bandwidth field for every non-Flex backend.
    //
    // Fire-and-forget like every DOWN verb. hz is a REQUEST: a backend whose
    // hardware offers a fixed set of rates snaps to the nearest one it can
    // actually run, and the span that resulted comes back via
    // panCenterBandwidthChanged. Callers must not assume the requested value was
    // taken — that assumption is what this verb exists to remove.
    //
    // Default no-op: a Flex radio owns its pan geometry and is driven by
    // "display pan set … bandwidth=" wire text, so FlexBackend has nothing to do
    // here.
    virtual void setPanBandwidth(const QString& panId, double hz)
    {
        Q_UNUSED(panId);
        Q_UNUSED(hz);
    }

    // Receive RF gain for a panadapter, in dB.
    //
    // The sibling of setPanCenter/setPanBandwidth, and it exists for the same
    // reason: a backend whose gain lives in a hardware register (the HL2's
    // AD9866 LNA, 0x0a) cannot be driven by "display pan set … rfgain=" wire
    // text, so the ANT panel's RF Gain slider reached nothing and the operator
    // had to RECONNECT to change gain — on a direct-sampling receiver, where a
    // strong band clips the converter and there is no AGC in front of it.
    //
    // gainDb is the operator's value in the range the backend itself advertised
    // via panRfGainInfoChanged. A backend clamps rather than refuses: the
    // control is continuous, and a silently ignored end-of-travel is worse than
    // a value that stops moving. What the hardware took comes back on
    // panRfGainChanged.
    //
    // Default no-op: a Flex radio takes rfgain as wire text, so FlexBackend has
    // nothing to do here.
    virtual void setPanRfGain(const QString& panId, int gainDb)
    {
        Q_UNUSED(panId);
        Q_UNUSED(gainDb);
    }

    // The discrete front-end stages above. `step` indexes the label list the
    // backend published; a backend clamps rather than refuses, exactly as
    // setPanRfGain does.
    //
    // Default no-op AND no capability flag: the empty label list a backend
    // publishes by default already hides the control, so a family without these
    // stages needs no declaration and cannot be asked for one.
    virtual void setPanPreamp(const QString& panId, int step)
    {
        Q_UNUSED(panId);
        Q_UNUSED(step);
    }
    virtual void setPanAttenuator(const QString& panId, int step)
    {
        Q_UNUSED(panId);
        Q_UNUSED(step);
    }
    virtual void setSliceRxAntenna(int sliceId, const QString& antenna)
    {
        Q_UNUSED(sliceId);
        Q_UNUSED(antenna);
    }

    // How often the operator wants panadapter frames, in frames per second.
    //
    // For a backend that streams cooked spectra there is no radio-side display
    // engine to ask, so the Display->FFT FPS slider has nowhere to go — the
    // Flex wire text it used to emit reached nothing — and the frame rate
    // defaults to the IQ sample rate over the FFT size, which tracks the
    // operator's ZOOM instead of their slider. Such a backend caps its own
    // production here, at the source, where the FFT can be skipped rather than
    // computed and thrown away.
    //
    // Default no-op: a Flex radio's own display engine paces its frames.
    virtual void setPanFrameRate(const QString& panId, int fps)
    {
        Q_UNUSED(panId);
        Q_UNUSED(fps);
    }

    // ---- per-slice audio ----
    //
    // A Flex mixes its slices ON THE RADIO, so these are wire commands to it and
    // these defaults are never reached. A backend that demodulates on this host
    // has to apply them in its own mixer, and without that the operator's mute
    // moves the fader while the audio keeps playing.
    //
    // gain and pan are 0..100 to match SliceModel's own range (pan: 0 left,
    // 50 centre, 100 right) rather than introducing a second scale at the seam.
    virtual void setSliceAudioMute(int sliceId, bool mute)
    {
        Q_UNUSED(sliceId);
        Q_UNUSED(mute);
    }
    virtual void setSliceAudioGain(int sliceId, int gainPercent)
    {
        Q_UNUSED(sliceId);
        Q_UNUSED(gainPercent);
    }
    virtual void setSliceAudioPan(int sliceId, int panPercent)
    {
        Q_UNUSED(sliceId);
        Q_UNUSED(panPercent);
    }

    // Move transmit to this slice. A radio with one transmitter and several
    // receivers has to MOVE it — retarget the TX oscillator, mode and passband —
    // rather than set a per-slice flag, so this is a verb and not a setter with
    // a bool. There is no "stop being the TX slice": transmit always lives
    // somewhere, and it is cleared only by another slice taking it.
    virtual void setTxSlice(int sliceId) { Q_UNUSED(sliceId); }

    // Make this the ACTIVE slice — the one the client's shared controls act on.
    // Distinct from setTxSlice: listening on one slice while transmitting on
    // another is normal, so selecting a pane must not drag transmit with it.
    //
    // A Flex arbitrates this itself (`slice set N active=1`) and echoes the
    // deselection of the previous slice back, so this default is never reached
    // there. A backend with no such echo has to clear the old one itself, or
    // every slice ever selected stays active and "the active slice" stops being
    // a single answer.
    virtual void setActiveSlice(int sliceId) { Q_UNUSED(sliceId); }

    // ---- panadapter lifecycle ----
    //
    // Bring up / tear down a panadapter (and, on a backend where a pan IS a
    // receiver, the receiver behind it). Return true if the backend took
    // ownership of the request; the new or removed pan is then reported through
    // the normal signals — panCenterBandwidthChanged and sliceChanged for a
    // creation, panRemoved for a teardown — exactly as at connect.
    //
    // Default FALSE, meaning "not mine". A Flex creates pans with its own wire
    // commands (`display panafall create`) and RadioModel keeps doing that; only
    // a backend that owns its own receivers needs these.
    //
    // Deliberately NOT a count setter. "Add a panadapter" is the operator's
    // actual intent and it is what the UI offers; a setReceiverCount(n) would
    // make every caller compute n from the current state and race anything else
    // that changed it.
    virtual bool createPanadapter() { return false; }
    virtual bool removePanadapter(const QString& panId)
    {
        Q_UNUSED(panId);
        return false;
    }

    // ── Manual notch filters ──────────────────────────────────────────────
    //
    // A notch is a null parked on an interferer at an ABSOLUTE RF frequency,
    // and it stays there while the operator tunes — a Flex calls this a TNF.
    // Whether it is realized in the radio (Flex) or in host DSP (HL2, where
    // the protocol carries no DSP at all) is exactly what this seam hides.
    //
    // IDS ARE ASSIGNED BY THE BACKEND, not chosen by the caller, which is why
    // createNotch() takes no id and returns nothing. A Flex mints the id in the
    // radio and reports it back in status; a host-DSP backend mints its own.
    // Either way the caller learns the id from notchChanged() and uses it for
    // every later edit. Requiring the caller to pick would force it to guess
    // what the radio will do, and two clients on the same Flex would collide.
    //
    // Default no-ops. A backend with no notch engine declares
    // capabilities().maxNotchFilters = 0 and the UI does not offer the control
    // at all, so these are never reached rather than silently doing nothing.
    virtual void createNotch(double centerHz, double widthHz)
    {
        Q_UNUSED(centerHz);
        Q_UNUSED(widthHz);
    }
    // Move, resize, or otherwise change an existing notch. A delta rather than
    // a fixed argument list for two reasons: it PERMITS a centre+width change
    // to rebuild the filter mask once instead of twice (no caller does that
    // yet — see NotchDelta.h), and the
    // Flex-only fields (depth, permanent) can then ride along without a
    // host-DSP backend having to pretend it understands them.
    virtual void setNotch(int notchId, const AetherSDR::NotchDelta& delta)
    {
        Q_UNUSED(notchId);
        Q_UNUSED(delta);
    }
    virtual void removeNotch(int notchId)
    {
        Q_UNUSED(notchId);
    }
    // Global bypass for every notch at once, the equivalent of a Flex
    // tnf_enabled. Individual notches keep their own active flag underneath.
    virtual void setNotchesEnabled(bool on)
    {
        Q_UNUSED(on);
    }

    // TX keying intent. The decision to allow keying is made ABOVE this seam by
    // the engine guard (RFC §6, single-holder lock + capability check); the
    // backend only translates an already-authorized intent to its mechanism
    // (command verb, in-stream bit, hardware line). A backend whose
    // capabilities().canTransmit is false implements this as a no-op.
    virtual void setKeying(bool key) = 0;

    // Let receive audio through WHILE TRANSMITTING.
    //
    // Receive audio is normally muted on transmit — the radio hears its own
    // signal at enormous strength, and unmuted it is fuzz on a carrier and an
    // acoustic feedback loop on voice. That mute is for the operator's comfort,
    // not for correctness.
    //
    // A diagnostic needs the opposite: demodulating our OWN transmission is the
    // only self-contained way to check the sideband convention, because the
    // panadapter reads raw wire order and therefore agrees with the transmitter
    // by construction, while the demodulator applies the receive conjugation and
    // WDSP's sideband selection independently. That distinction is what a whole
    // bring-up turned on — see docs/HERMES.md 14.6 and 15.5.
    //
    // Default OFF. Turning it on outside a measurement will be unpleasant.
    virtual void setTxAudioMonitor(bool on) { Q_UNUSED(on); }

    // The operator-facing radio MON switch and level. This is deliberately
    // separate from the diagnostic receive-during-TX gate above.
    virtual void setTxMonitor(bool on, int level)
    {
        Q_UNUSED(on);
        Q_UNUSED(level);
    }

    // Tune carrier on/off, at the operator's TUNE power (percent, 0..100).
    //
    // Flex takes "transmit tune N" as a text command, so FlexBackend has nothing
    // to do here. A backend that generates its own carrier implements it.
    //
    // tunePowerPercent is passed because a host-modulated backend has no other
    // route to it: it raises its own carrier and sets its own drive, so without
    // the value here it can only transmit at whatever setTxPower() last pushed —
    // the RF Power slider. That made TUNE key at FULL power for anyone running
    // RF 100 / Tune 10, which is the opposite of what the control is for.
    // Defaulted so existing implementations stay source-compatible.
    virtual void setTune(bool on, int tunePowerPercent = -1)
    {
        Q_UNUSED(on);
        Q_UNUSED(tunePowerPercent);
    }

    // Transmit power as a percentage, 0..100.
    //
    // Flex takes this as a text command from TransmitModel, so FlexBackend has
    // nothing to do here. A backend that owns its own drive register (HL2)
    // implements it.
    virtual void setTxPower(int percent) { Q_UNUSED(percent); }

    // The operator's CW pitch, in Hz (TransmitModel's range: 100..6000).
    //
    // A sidetone setting on a radio that keys itself; a TUNING setting on a
    // radio whose demodulator we own. The CW convention every client shares is
    // that the marker sits on the signal and the receiver produces the pitch
    // from a BFO, so a host-demodulating backend has to know the pitch to place
    // its passband at all — see Hl2Backend::cwBfoHz(). Get this wrong and the
    // panadapter's CW passband draws a whole pitch away from the marker while
    // the transmitter keys on the marker itself.
    //
    // Default no-op: a Flex owns its own DSP and takes `cw pitch` as text from
    // TransmitModel, so this seam would be a second, redundant opinion.
    virtual void setCwPitch(int hz) { Q_UNUSED(hz); }

    // The speech processor, as the operator sees it: an enable plus one of
    // three presets (0 = NOR, 1 = DX, 2 = DX+).
    //
    // That shape is FlexRadio's, and it is not universal. On a radio with its
    // own compressor the two halves are SEPARATE registers — the IC-705 wants
    // 16 44 for the enable and 14 0E for how hard — so a backend receives both
    // together and decides how to spend them. Default no-op: Flex takes this as
    // text from TransmitModel, and a host-modulating backend runs its own
    // compressor in our DSP instead.
    virtual void setSpeechProcessor(bool on, int level)
    {
        Q_UNUSED(on);
        Q_UNUSED(level);
    }

    // VOX — the enable, the trigger threshold and the hang time.
    //
    // Three together for the same reason the speech processor's two are: they
    // are one control to the operator and separate registers on the radio, and
    // a backend receives all of them so it can decide how to spend them. An
    // IC-705 has 16 46 for the enable and 14 16 / 14 17 for level and delay; a
    // radio with fewer ignores what it does not have.
    //
    // Default no-op: a Flex takes VOX as text from TransmitModel.
    virtual void setVox(bool on, int level, int delayMs)
    {
        Q_UNUSED(on); Q_UNUSED(level); Q_UNUSED(delayMs);
    }

    // The ANTENNA TUNER, and NOT setTune().
    //
    // These are two different things that both say "tune", and conflating them
    // is how an operator ends up running a matching cycle on an ATU that may
    // not be attached. setTune() emits a steady carrier for adjusting an
    // external amplifier; this runs the radio's own matching cycle.
    //
    // `start` true begins a cycle, false bypasses. What the tuner then did
    // comes back on transmitChanged's ATU fields.
    //
    // KEYS THE TRANSMITTER on a radio with a real ATU, so it sits behind the
    // same TX gate as every other keying intent.
    virtual void setAtu(bool start) { Q_UNUSED(start); }

    // Receive and transmit incremental tuning. Hz relative to the VFO.
    //
    // Two enables and one offset, because that is the shape every radio that
    // has them uses — including the IC-705, where they are 21 01, 21 02 and
    // 21 00. A radio without RIT simply does not implement these.
    // RECEIVE DSP THE RADIO'S OWN FIRMWARE RUNS — the set gated by
    // capabilities().hasRadioSideDsp.
    //
    // These arrived late, and their absence was a silent hole rather than a
    // missing feature. SliceModel drove every one of them by emitting FlexRadio
    // wire text ("slice set 0 nr=1"), which IS the command on a Flex and is
    // discarded everywhere else — and with no verb here, no other backend could
    // implement them however much it wanted to. So hasRadioSideDsp was a
    // capability that promised something the seam had no way to deliver.
    //
    // Enable and level travel together: a radio with a level register generally
    // needs both to make either meaningful, and splitting them is how a toggle
    // lands before the level it implies. A backend without a level ignores it.
    virtual void setSliceNoiseReduction(int sliceId, bool on, int level)
    {
        Q_UNUSED(sliceId); Q_UNUSED(on); Q_UNUSED(level);
    }
    virtual void setSliceNoiseBlanker(int sliceId, bool on, int level)
    {
        Q_UNUSED(sliceId); Q_UNUSED(on); Q_UNUSED(level);
    }
    virtual void setSliceAutoNotch(int sliceId, bool on)
    {
        Q_UNUSED(sliceId); Q_UNUSED(on);
    }
    // The radio's single operator-placed notch — capabilities().hasManualNotch.
    //
    // `position` is 0..100 across the receive passband, NOT a frequency. That is
    // the shape the register has: an IC-705 takes 14 0D as 0000..0255 spanning
    // whatever the current filter is, so the notch moves with the passband and
    // an absolute frequency would have to be re-derived on every filter change.
    // A backend whose notch IS frequency-placed converts here, where it knows
    // its own passband, rather than making every caller do it.
    //
    // Enable and position travel together for the same reason the noise verbs
    // do: turning the notch on without placing it puts it wherever the radio
    // last left it, which is not where the operator's slider is.
    virtual void setSliceManualNotch(int sliceId, bool on, int position)
    {
        Q_UNUSED(sliceId); Q_UNUSED(on); Q_UNUSED(position);
    }
    virtual void setSliceSquelch(int sliceId, bool on, int level)
    {
        Q_UNUSED(sliceId); Q_UNUSED(on); Q_UNUSED(level);
    }

    virtual void setRitEnabled(bool on) { Q_UNUSED(on); }
    virtual void setXitEnabled(bool on) { Q_UNUSED(on); }
    virtual void setRitOffset(int hz) { Q_UNUSED(hz); }

    // The TRANSMIT offset, separately from the receive one.
    //
    // Defaults to setRitOffset() because the radio this seam was first shaped
    // against has ONE shared register: an IC-705 keeps the shift in 21 00 and
    // uses 21 01 / 21 02 only to choose whether it applies to receive, transmit
    // or both. A radio with two independent registers — a Flex has separate
    // rit_freq and xit_freq — overrides this and stops the two aliasing.
    //
    // Split out because without it the seam had two enables and one offset, and
    // an XIT intent silently wrote the RIT register on every backend rather
    // than only on the one where that is the truth.
    virtual void setXitOffset(int hz) { setRitOffset(hz); }

    // Transmit audio passband, in Hz above the carrier — the Phone applet's TX
    // low-cut and high-cut.
    //
    // Same division as setTxPower: a Flex takes this as `transmit set
    // filter_low=/filter_high=` from TransmitModel, so FlexBackend has nothing
    // to do here; a backend that owns its own modulator implements it.
    //
    // ALWAYS POSITIVE AUDIO Hz, on both sidebands. The sideband is not chosen by
    // the sign of this passband — a host modulator picks it in the modulator
    // itself — so LSB takes exactly the same numbers as USB. Reflecting these
    // for LSB would transmit on the wrong sideband.
    //
    // Once called, the operator's passband OWNS the modulator: a backend must
    // not let a per-mode default overwrite it on the next mode change, or the
    // control works until the operator touches anything else.
    virtual void setTxFilter(int lowHz, int highHz)
    {
        Q_UNUSED(lowHz);
        Q_UNUSED(highHz);
    }

    // Microphone gain, 0..100, as the Phone applet's MIC slider means it.
    //
    // Same seam and same reason as setTxFilter() above: on a Flex the slider's
    // `transmit set miclevel=` reaches the radio's own preamp, but a backend
    // that modulates on this host has no command plane to receive it and the
    // verb is dropped. Without this the slider was inert on the HL2 — moving it
    // end to end changed nothing on the air, which reads as a dead control
    // rather than as a control aimed at hardware that is not there.
    //
    // A backend that takes this owns the gain: nothing else scales the mic on
    // its behalf, so ignoring the call means the operator has no mic gain at all.
    virtual void setMicGain(int level)
    {
        Q_UNUSED(level);
    }

    // Processed transmit audio, int16 interleaved stereo at sampleRateHz.
    //
    // For backends that modulate on the host (HL2). A Flex radio does its own
    // modulation from mic or DAX, so FlexBackend ignores this — hence a default
    // no-op rather than a pure virtual.
    //
    // The audio is already shaped: AudioEngine has applied the test tone,
    // compressor and EQ before this point. That is deliberate — the TONE button,
    // the microphone and any future source all reach the air through ONE path,
    // so what the operator monitors is what gets transmitted.
    //
    // `clientLeveled` is true when the audio came from an external TCI/DAX
    // client rather than the mic chain or the engine's own generators. The
    // sender of such audio has already applied its own level control, so a
    // host-modulating backend must not run makeup gain (ALC) over it (#4796).
    // No default argument — defaults on virtuals bind statically, and the
    // override a caller actually reaches would quietly diverge from it.
    virtual void submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz,
                               bool clientLeveled)
    {
        Q_UNUSED(int16Stereo);
        Q_UNUSED(sampleRateHz);
        Q_UNUSED(clientLeveled);
    }

    // ---- diagnostics ----
    //
    // A snapshot of whatever health/status registers this backend can report:
    // converter overload, transmit FIFO depth, thermal, firmware revision, link
    // counters. Purely for display — nothing in the app makes a decision from
    // it, which is why it is a plain map rather than a typed delta.
    //
    // SYNCHRONOUS, and that is not a shortcut. Every value here is already
    // cached in the backend from telemetry the radio sends unprompted, so there
    // is no wire round-trip to await. Making it async would mean a dialog that
    // renders empty and fills in later, for data that is sitting in memory.
    // A backend whose values live on a worker thread must cache them on this
    // one (see Hl2Backend) rather than reaching across.
    //
    // Two parallel outputs so the dialog does not have to know the vocabulary:
    // `values` is the data, and `order` lists the keys in the sequence they
    // should be displayed, so a backend controls its own grouping. Keys absent
    // from `values` are rendered as "not reported" rather than as zero — on a
    // health readout the difference between "0" and "we never heard" is the
    // whole point.
    struct HealthSnapshot {
        QVariantMap values;
        QStringList order;
        // Section headings, keyed by the value-key they precede.
        QMap<QString, QString> sections;
        // Human-readable labels, keyed by value-key. A key with no label is
        // displayed under its own name.
        QMap<QString, QString> labels;
        [[nodiscard]] bool isEmpty() const { return order.isEmpty(); }
    };
    virtual HealthSnapshot healthSnapshot() const { return {}; }

    // The state of the TRANSPORT carrying this radio's streams, as opposed to
    // the state of the radio itself (which is healthSnapshot's job).
    //
    // The network readouts — the title-bar heartbeat, the status-bar Network
    // field, the whole Network Diagnostics dialog — were built against the Flex
    // stack and read their numbers off a RadioConnection (TCP ping RTT) and a
    // PanadapterStream (VITA-49 byte and sequence counters). A family that owns
    // neither has both of those as nullptr, so every one of those surfaces read
    // a hard zero: the heartbeat never left its pre-connect amber, and the
    // diagnostics pane reported a connected radio pushing 0 kbps with 0 packets.
    // Not degraded — structurally blank, on a link that was working fine.
    //
    // So the counters have to come from whoever owns the socket, which is the
    // backend. This is the neutral shape of that, and it is deliberately about
    // a TRANSPORT rather than about UDP or VITA-49: a backend gets to report
    // the subset it can actually measure, and says so.
    //
    // `reported` false — the default, and what every backend that does not
    // override this answers — means "I measure no transport", and the consumer
    // keeps whatever source it was already using. That is what makes this
    // additive: the Flex path never sees a LinkStats at all.
    struct LinkStats {
        // False: this backend measures nothing; ignore every field below.
        bool reported = false;
        // Traffic arrived from the radio since the PREVIOUS snapshot. This is
        // the proof-of-life the heartbeat indicator runs on — a link that is
        // bound and counting but has gone silent must read as dead, and a
        // cumulative counter alone cannot say that.
        bool alive = false;

        qint64  rxBytes = 0;
        qint64  txBytes = 0;
        quint64 rxPackets = 0;         // cumulative, this session
        quint64 rxPacketsLost = 0;     // cumulative sequence gaps, this session

        // Negative means NOT MEASURED, which is not the same as zero and must
        // not render as one. A stream-only transport has no request/response
        // exchange to time, so its RTT is genuinely unknown — printing "< 1 ms"
        // there would be the readout inventing a measurement it never took.
        int rttMs    = -1;
        int jitterMs = -1;
        int gapMs    = -1;
        int gapMaxMs = -1;

        // "ip:port" of the local socket, empty when not bound.
        QString localEndpoint;
    };
    virtual LinkStats linkStats() const { return {}; }

    // ---- vendor extensions (namespaced, capability-advertised) ----
    // Vendor-specific verbs that are NOT part of the core profile. Clients
    // discover available namespaces via capabilities().extensionNamespaces.
    //
    // Fire-and-forget like every other DOWN verb: the result of a real device
    // command (ATU tune, amp state, …) arrives later on the wire, so it comes
    // back asynchronously via extensionResult(requestId, …) / extensionError.
    // The caller (above the seam) mints requestId and correlates the reply; a
    // requestId of 0 means "no reply expected". A synchronous QVariant return
    // would have to block or fabricate a local value against an async backend.
    virtual void invokeExtension(const QString& ns, const QString& verb,
                                 quint64 requestId, const QVariant& arg = {}) = 0;

signals:
    // ---- connection state UP ----
    void connected();
    void disconnected();
    void connectionError(const QString& reason);

    // A problem with the RADIO'S CONFIGURATION that the operator should fix,
    // but which does not end the session. Distinct from connectionError, which
    // every consumer treats as fatal: RadioModel starts its reconnect timer on
    // it unconditionally, so using that channel for advice tears down a working
    // link and then does it again on the next attempt — a permanent reconnect
    // loop whose cause reads as a helpful message. That is exactly what an
    // IC-9700 with MOD Input set to USB did: connect, warn, drop, repeat every
    // 5 s, with the radio itself perfectly healthy.
    //
    // If it does not stop the radio working, it belongs here.
    void configurationWarning(const QString& message);

    void capabilitiesChanged();

    // A fresh transport snapshot. Emitted on a FIXED cadence while connected,
    // not when traffic arrives — the tick has to keep coming after the radio
    // goes quiet, because "nothing arrived this second" is the observation the
    // heartbeat's alarm path is waiting for. A backend that emits only on
    // receive can never report its own silence.
    void linkStatsUpdated(const IRadioBackend::LinkStats& stats);

    // ---- vendor-extension replies UP (correlate to invokeExtension) ----
    // The async result of an invokeExtension() call, keyed by the caller's
    // requestId. A backend that completes locally may emit this synchronously;
    // one speaking a wire protocol emits it when the device answers.
    void extensionResult(quint64 requestId, const QVariant& result);
    void extensionError(quint64 requestId, const QString& reason);

    // ---- normalized model state UP (RadioModel connects these to its
    //      sub-models; Q2) — the key/value shape mirrors the models' fields ----
    // Normalized slice-status delta (aetherd RFC 2.3). Typed + compiler-checked:
    // the backend populates only the fields the wire reported, the model applies
    // exactly those. Replaces the prior stringly-keyed QVariantMap payload.
    void sliceChanged(int sliceId, const SliceDelta& delta);
    // On a backend where a slice IS a receiver, closing the receiver has to
    // retire BOTH this and panRemoved. Emitting only panRemoved left the
    // SliceModel behind, still naming a pan id that no longer existed — and
    // nothing looked broken until the next create, when the capacity guard
    // compared a slice count that never fell against maxSlices() and reported
    // "Slice capacity is full" on a radio with one receiver running.
    void sliceRemoved(int sliceId);
    void meterUpdate(const QString& meterId, double value);

    // Normalized transmit-status delta (aetherd RFC 2.3 — TransmitModel
    // touchpoint). Typed + compiler-checked; the backend populates only the
    // fields the wire reported (across the transmit / interlock / ATU / APD /
    // APD-sampler status planes) and RadioModel drives the TransmitModel.
    void transmitChanged(const TransmitDelta& delta);

    // Normalized power-amplifier status delta (aetherd 2.4 — AmpModel decode
    // split, #4094). Typed + present-only; the backend translates the SmartSDR
    // "amplifier" wire and AmpModel applies the state machine. Command/encode is
    // the neutral AmpModel::operateRequested intent, translated back to the wire
    // by invokeExtension("flex", "amp.operate", …) (#4094).
    void amplifierChanged(const AmpDelta& delta);

    // Normalized antenna-tuner status delta (aetherd 2.4 — TunerModel decode
    // split, #4092). Typed + present-only; the backend translates the SmartSDR
    // "atu"/"amplifier"(TunerGeniusXL) wire, TunerModel applies the change-gated
    // state. Command/encode is TunerModel's neutral operate/bypass/autotune
    // intents, translated by invokeExtension("flex", "tuner.*", …) (#4092).
    void tunerChanged(const TunerDelta& delta);

    // Normalized radio-global status delta (aetherd RFC 2.3 — RadioModel
    // residual). Typed + compiler-checked; the backend populates only the fields
    // the wire reported and RadioModel applies them + its own orchestration.
    void radioChanged(const RadioDelta& delta);

    // Normalized GPS status delta (aetherd RFC 2.3 — RadioModel residual). The
    // backend tokenizes the vendor GPS status line into a present-only GpsDelta;
    // RadioModel applies it and emits gpsStatusChanged.
    void gpsChanged(const GpsDelta& delta);

    // Normalized memory-slot status (aetherd RFC 2.3 — RadioModel residual),
    // keyed by slot index. The backend decodes the vendor memory-status kv-set;
    // RadioModel applies it to MemoryEntry (text sanitisation is a model
    // concern) or drops the slot when delta.removed is set.
    void memoryChanged(const MemoryDelta& delta);

    // Normalized profile status (aetherd RFC 2.3 — RadioModel residual). The
    // backend parses the vendor "profile <type> …" status (list/current + the
    // database importing/exporting flags); RadioModel routes it to TransmitModel
    // tx/mic profiles, the global-profile list, or the import/export flags.
    void profileChanged(const ProfileDelta& delta);

    // Meter definition catalog (aetherd RFC 2.3 — MeterModel touchpoint). The
    // backend decodes the vendor meter-status wire format into a typed MeterDef;
    // RadioModel hands it straight to MeterModel::defineMeter(). Fields the wire
    // did not report keep their MeterDef defaults (present-only on the decode
    // side). The meter *values* stream on the data plane (VITA-49), separate.
    // (#4070: typed payload — replaces the prior stringly-keyed QVariantMap.)
    void meterDefined(const MeterDef& def);
    void meterRemoved(int index);
    // Panadapter core display state (universal — every family has a pan center
    // and span). The backend decodes it from vendor status; RadioModel drives
    // the PanadapterModel. panId is the pan's identifier (opaque to the model).
    // (aetherd RFC 2.3 — first converted touchpoint; the template the other
    // universal pan fields + the other mixed models follow.)
    void panCenterBandwidthChanged(const QString& panId,
                                   double centerMhz, double bandwidthMhz);

    // A pan the backend owned is GONE. Emitted after removePanadapter() has
    // actually torn the receiver down, so the model removes the pane only once
    // the thing behind it has stopped — never optimistically, which would leave
    // a receiver streaming into a pane nobody is listening to.
    void panRemoved(const QString& panId);

    // A notch was created or changed, reported with the id the BACKEND assigned
    // (see createNotch above). This is how the caller learns an id at all, so a
    // backend that mints its own must emit it after every create — including
    // when it rejects one, by simply not emitting.
    //
    // FlexBackend does NOT emit these: a Flex reports TNFs as `tnf <id> …`
    // status on its command plane, which RadioModel already decodes. Only a
    // backend whose notches exist nowhere but in this process needs to say so.
    void notchChanged(int notchId, const AetherSDR::NotchDelta& delta);
    void notchRemoved(int notchId);

    // ONE SLICE's demodulated RX audio, tagged with the slice it came from.
    //
    // The sibling of audioFrameReady, which is the SPEAKER feed — already mixed
    // down to a single stream, with per-slice mute, level and balance applied.
    // That is the right shape for the speaker and the wrong shape for every
    // per-slice consumer: a TCI receiver channel, a decoder, a recorder. They
    // each need one slice's audio, and asking them to un-mix a sum is not
    // possible.
    //
    // Emitted PRE-mute, PRE-gain and PRE-balance, deliberately. On a Flex these
    // consumers are fed by DAX, which is a separate plane from the speaker —
    // muting a slice silences the monitor and the DAX stream a decoder is using
    // keeps flowing. Applying the speaker's mute here would make muting a slice
    // stop WSJT-X decoding on it, which is not what the control means.
    //
    // Flex does NOT emit this: its per-slice audio already arrives as DAX
    // channels, which are per-slice by construction. Only a backend that
    // demodulates in this process has to say which slice a buffer belongs to.
    void sliceAudioFrameReady(int sliceId, const QByteArray& pcm);


    // The pan's front end is WIDE: the hardware band filter cannot serve every
    // active receiver at once, so it has been bypassed. On a Flex this is what
    // a pan sharing an ADC across bands reports; on an HL2 it is the
    // agree-or-bypass policy in applyBandFilter() becoming visible.
    //
    // Radio-wide in cause but reported PER PAN, because that is where the
    // operator sees it and because a future radio could bypass per receiver.
    void panWideChanged(const QString& panId, bool wide);

    // Panadapter display level range (universal — the Y-axis geometry that
    // pairs with center/bandwidth's X-axis). Unlike center/bandwidth, dBm is
    // signed, so the "unchanged" sentinel for an omitted field is NaN, not a
    // negative value — the backend carries NaN for whichever of min/max the
    // wire did not report. (aetherd RFC 2.3 — second converted universal pan
    // field, following the center/bandwidth template.)
    void panRangeChanged(const QString& panId, double minDbm, double maxDbm);

    // The span limits this pan can actually be zoomed between (universal — every
    // family has a widest and narrowest window). Reported by the backend because
    // only the backend knows: for one that owns a DDC the limits are its
    // available decimation rates, which no model-name table can predict.
    //
    // This is what keeps the zoom clamp honest. Before it, the GUI clamped every
    // radio against a FlexLib model table that falls through to 5.4 MHz for any
    // model string it doesn't recognise — so an HL2 delivering 384 kHz could be
    // zoomed 14x wider than its own data, and the uncovered spectrum rendered as
    // black bars either side of the trace. A backend that reports its real limits
    // gets a zoom that stops where the data stops.
    //
    // Both bounds in MHz. A backend that doesn't know (or whose hardware has no
    // meaningful limit) simply never emits this, and the GUI keeps its previous
    // model-derived clamp — so this is additive for Flex.
    void panBandwidthLimitsChanged(const QString& panId,
                                   double minMhz, double maxMhz);

    // Panadapter RF gain (universal — every family has an RX gain control; the
    // range/step are family-specific and reported via RadioCapabilities). The
    // backend decodes it from vendor status; RadioModel drives the pan.
    void panRfGainChanged(const QString& panId, int gain);
    // Operating state moved (frequency, mode, passband, rate, gain, drive) —
    // fetch it with currentOperatingState(). Emitted only by backends with a
    // non-empty clientSettingsDomains declaration; rate-limiting is the
    // subscriber's job (RadioModel debounces).
    void operatingStateChanged();

    // The RF-gain range and step this pan actually offers, in dB.
    //
    // Reported by the backend for the same reason the span limits are: only the
    // backend knows. Flex learns it by asking the radio ("display pan
    // rfgain_info"), which is a Flex command and answers nothing on any other
    // family — so without this an HL2 kept the model's Flex-shaped -8..+32 in
    // 8 dB steps while its AD9866 LNA actually spans -12..+48 in 1 dB steps,
    // and two thirds of the available gain was unreachable from the slider.
    //
    // A backend that doesn't know simply never emits this and the model keeps
    // its existing defaults, so this is additive for Flex.
    // `unitSuffix` is what the readout appends to the number — " dB" for a real
    // gain register, "%" for a radio whose RF gain is an opaque 0..255 scale
    // with no published dB mapping. Defaulted so every existing emitter is
    // unchanged, and so a backend that stays silent still reads as dB.
    //
    // It exists because the readout used to hardcode " dB" while the Icom
    // backend was pointing this slider at a THREE-POSITION PREAMP: the operator
    // saw "0 dB / 1 dB / 2 dB" for what the radio calls OFF / P.AMP1 / P.AMP2,
    // and none of those numbers was a decibel of anything.
    void panRfGainInfoChanged(const QString& panId, int low, int high, int step,
                              const QString& unitSuffix = QStringLiteral(" dB"));

    // DISCRETE receive front-end stages, which a continuous gain slider cannot
    // represent honestly: a preamp with named positions, and a stepped
    // attenuator. Both are "which position", never "how much".
    //
    // `labels` names every position in order, and its SIZE is the control's
    // range — {"OFF", "P.AMP1", "P.AMP2"} is a three-position preamp addressed
    // as 0, 1, 2. An EMPTY list means the radio has no such stage and the
    // control does not appear; that is the default for every backend, so this
    // is additive.
    //
    // Names, not numbers, because the numbers are not physical. An IC-705's
    // preamp positions have no published gain figures and its attenuator has
    // exactly one step (20 dB, HF and 50 MHz only) — so a slider reading
    // "0/1/2" or "0-1" invents a scale the radio does not have. What the
    // hardware took comes back on panPreampChanged / panAttenuatorChanged.
    void panPreampInfoChanged(const QString& panId, const QStringList& labels);
    void panPreampChanged(const QString& panId, int step);
    void panAttenuatorInfoChanged(const QString& panId, const QStringList& labels);
    void panAttenuatorChanged(const QString& panId, int step);

    // Panadapter antenna selection (universal). Two signals because the wire may
    // report the selected RX antenna and the available list independently.
    void panRxAntennaChanged(const QString& panId, const QString& antenna);
    void panAntennaListChanged(const QString& panId, const QStringList& antennas);

    // Waterfall line duration in ms (universal display timing). Decoded from the
    // waterfall-status plane; RadioModel drives the pan's waterfall model state.
    void panWaterfallLineDurationChanged(const QString& panId, int ms);

    // Vendor-specific status data that is NOT part of the core profile — the
    // namespaced *extension* channel (aetherd RFC §5.5). A client that doesn't
    // understand `ns` ignores it; `kind` names the event within the namespace
    // and `fields` carries only the keys the wire actually reported. This is the
    // status counterpart to invokeExtension's request/reply.
    void extensionStatus(const QString& ns, const QString& kind,
                         const QVariantMap& fields);

    // ---- data plane UP (RFC §4.2) ----
    // Declared here so backends have a normalized outlet for spectrum/waterfall/
    // audio; the concrete zero-copy/binary frame formats are step-4 work. Until
    // then a backend may relay the existing in-tree frame types.
    void spectrumFrameReady(int panId, const QByteArray& frame);
    void waterfallRowReady(int panId, const QByteArray& row);
    void audioFrameReady(const QByteArray& pcm);
};

}  // namespace AetherSDR

// linkStatsUpdated is direct-connected today (Hl2Backend's cadence timer lives
// on the same thread as its consumer), but a backend whose socket owner emits
// it from a worker thread would need the queued path — which silently drops the
// signal unless the type is registered. Declared here so that stays a
// non-event.
Q_DECLARE_METATYPE(AetherSDR::IRadioBackend::LinkStats)
