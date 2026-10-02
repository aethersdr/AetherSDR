#pragma once

#include "IndependentTxControl.h"

#include "core/RadioSettingsIdentity.h"
#include "core/RadioSettingsScope.h"
#include "core/TxCoordinator.h"
#include "core/PcmFrame.h"

#include <map>

#include <QByteArray>
#include <QLoggingCategory>
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
#include "core/backends/FrontEndOverload.h"
#include "core/backends/RadioCapabilities.h"
#include "core/backends/RestoredRadioState.h"
#include "core/backends/RadioDelta.h"
#include "core/backends/SliceDelta.h"
#include "core/backends/SpectrumCoverage.h"
#include "core/backends/TransmitDelta.h"
#include "core/backends/TunerDelta.h"
#include "core/backends/TxAudioSource.h"

namespace AetherSDR {

// Borrowed handle returned by autoRfGainControl(); see AutoRfGainControl.h.
class IAutoRfGainControl;

// Neutral, family-agnostic connect descriptor. Core fields cover the common
// case; vendor-specific parameters (SmartLink token, Kiwi endpoint path, …)
// ride in `params` so the interface never grows a per-vendor connect signature.
struct RadioConnectRequest {
    QString host;
    quint16 port = 0;
    QString serial;         // when a family identifies radios by serial
    QVariantMap params;     // family-specific extras (namespaced by the backend)
    RadioSerialIdentity serialIdentity;
};

// Complete radio-owned memory state applied after the common frequency/mode
// fields. Keeping this as one value object prevents positional call sites from
// silently swapping the independent TX/RX tone and DTCS fields.
struct MemoryRecallDetails {
    int sliceId = -1;
    int filterPreset = 0;
    bool dataMode = false;
    QString direction;
    double offsetHz = 0.0;
    QString toneMode;
    double txToneHz = 0.0;
    double rxToneHz = 0.0;
    int dtcsCode = 23;
    bool dtcsTxReverse = false;
    bool dtcsRxReverse = false;
};

// The radio-facing seam of the engine (aetherd RFC §5.5). Everything that speaks
// a vendor wire protocol lives behind this interface; RadioModel sees only this,
// and each radio family is one implementor. RadioModel owns the sub-models; the
// backend drives them through the signals below. A receive-only family reports
// capabilities().canTransmit == false and makes setKeying() a guarded no-op.
// DSP location is invisible here: a thin protocol decoder and a raw-IQ backend
// with its own DSP chain emit the same normalized signals.
//
// ---- THREADING AND LIFETIME CONTRACT ----------------------------------------
// Every implementor honours these; backend_seam_affinity_test and
// backend_family_switch_test pin them. A backend that needs an exception changes
// this text first.
//
//  1. THE BACKEND OBJECT LIVES ON ITS OWNER'S THREAD — RadioModel's (GUI thread
//     in the app, main thread in aetherd). It is never moved, every virtual is
//     called there, and verbs need no locks against each other.
//  2. EVERY SEAM SIGNAL IS EMITTED FROM THAT THREAD, data plane
//     (audioFrameReady, sliceAudioFrameReady, spectrumFrameReady, meterUpdate)
//     and linkStatsUpdated included. A worker hops back first (queued connection
//     with `this` as context, or QMetaObject::invokeMethod(this, ...)); never
//     emit from a worker callback, a DirectConnection lambda on a worker sender,
//     or a std::function the worker invokes. Consumers connect with Auto.
//  3. WORKERS ARE THE BACKEND'S PRIVATE BUSINESS. Nothing above the seam may
//     observe, name or wait on them; consumers ask the backend (healthSnapshot(),
//     linkStats(), dspChains()) and it answers from cache. Sole transitional
//     exception: FlexBackend::connection()/panStream() and SimBackend's
//     equivalents, which RadioModel drives directly (including
//     BlockingQueuedConnection) until #5262 M4 / #5554 §2.6. No new call site may
//     join them, and every handler bound to them is generation-guarded per rule 5.
//  4. SEAM PAYLOADS ARE DECLARED AND REGISTERED IN ONE PLACE. Every type crossing
//     the seam (*Delta structs, MeterDef, LinkStats) has Q_DECLARE_METATYPE in
//     its header AND qRegisterMetaType in RadioModel's constructor. Qt 6 queued
//     delivery works without it; name-based paths (QMetaType::fromName, QVariant,
//     string SIGNAL/SLOT, QSignalSpy capture) need it.
//  5. TEARDOWN IS BOUNDED AND ORDERED. disconnectRadio() returns with no worker
//     able to reach a seam signal: sources stopped, threads joined (or handed to
//     a self-deleting reaper, as RtlSdrBackend does for a stuck open).
//     disconnected() is emitted exactly once, from the backend's thread,
//     before returning or asynchronously — never twice, never from a worker. The
//     destructor completes the same drain and never waits on a
//     BlockingQueuedConnection whose target may be waiting on this thread.
//     Disconnecting signals does not drop calls already queued, so
//     teardownBackend() bumps a generation counter and every handler bound to a
//     backend-owned object returns early on mismatch (RadioModel::setupBackend()).
//  6. A BACKEND EMITS NOTHING AFTER disconnected(). Frames a worker queued before
//     stopping are gated on the backend's own connected flag (see SimBackend's
//     audio forwards). sim_backend_test pins this.
//
// The interface grows one method at a time per the touchpoint burndown
// (docs/architecture/aetherd-touchpoints.md).

// Owned by the model, borrowed by a backend. See backends/OfflineHealthSource.h.
class IOfflineHealthSource;

class IRadioBackend : public QObject {
    Q_OBJECT

public:
    explicit IRadioBackend(QObject* parent = nullptr) : QObject(parent)
    {
        connect(this, &IRadioBackend::connected, this, [this] {
            m_slicePcm.clear();
            ++m_pcmSession;
            m_pcmLive = m_speakerPcm.start(PcmPurpose::Speaker, -1, {}, m_pcmSession);
            if (!m_pcmLive) {
                // Refusing here means total RX silence on this backend. Say so:
                // every downstream refusal is a silent return, so without this
                // the failure is indistinguishable from a dead radio.
                qWarning() << "IRadioBackend: speaker PCM producer refused to start for session"
                           << m_pcmSession << "- RX audio will be silent on this connection";
            }
        });
        connect(this, &IRadioBackend::disconnected, this, [this] {
            retirePcmStreams();
        });
        connect(this, &IRadioBackend::sliceRemoved, this, [this](int id) {
            m_slicePcm.erase(id);
        });
    }
    ~IRadioBackend() override = default;

    // Owner-thread retirement before disconnect/teardown can pump events.
    // Revokes already queued compatibility frames without touching the radio.
    void retirePcmStreams()
    {
        m_pcmLive = false;
        m_speakerPcm.invalidate();
        m_slicePcm.clear();
    }

    // ---- identity & capability (feeds the protocol `welcome`, §4.1) ----
    virtual RadioCapabilities capabilities() const = 0;

    // True when this backend delivers demodulated RX audio over the seam
    // (audioFrameReady) rather than through a Flex PanadapterStream. The RX-audio
    // wiring keys off this, never off a family name; answering wrong feeds the engine
    // twice (#4490). Not "has no PanadapterStream": the sim has both and answers true
    // because the seam is its real audio.
    virtual bool ownsRxAudio() const { return false; }

    // ---- connection lifecycle ----
    // Model-owned identity handoff for feature-document owners. A backend
    // must not reconstruct this scope from a later USB/discovery observation.
    virtual void configureSettingsScope(const RadioSettingsScope& scope, const RadioSerialIdentity& identity)
    { Q_UNUSED(scope); Q_UNUSED(identity); }
    // nullopt retains generic OperatingState ownership. A feature owner
    // returns its atomic-write result, including refusal, so a failed write
    // never falls through into a second overlapping writer.
    virtual std::optional<bool> storeOperatingState(const RadioSettingsScope& scope,
                                                   const RestoredRadioState& state)
    { Q_UNUSED(scope); Q_UNUSED(state); return std::nullopt; }

    // Typed restore handoff (RFC #4603 proposal B): called by RadioModel BEFORE
    // connectRadio(), only when this backend's clientSettingsDomains is non-empty.
    // The backend stashes what it wants, validates its own extension document here,
    // and applies it during connect/initial-push. Default no-op: a
    // radio-authoritative backend (Flex) never sees restored state.
    virtual void applyRestoredState(const RestoredRadioState& state)
    {
        Q_UNUSED(state);
    }
    virtual void connectRadio(const RadioConnectRequest& request) = 0;
    // The capture half of RadioStateMemory (RFC #4603 PR 3): a backend whose
    // declared clientSettingsDomains is non-empty reports its operating state
    // here on demand, and emits operatingStateChanged() (see signals) when it
    // moves. RadioModel debounces the signal and persists the snapshot via
    // storeOperatingState when handled, otherwise RadioStateMemory::store.
    virtual RestoredRadioState currentOperatingState() const { return {}; }
    virtual void disconnectRadio() = 0;
    virtual bool isConnected() const = 0;

    // ---- intents DOWN: canonical core-profile verbs (grow per burndown) ----
    // The backend translates each to its vendor wire protocol.
    // Opt-in for slice frequency/mode/filter, monitor gain/pan/mute and pan
    // center/bandwidth. Dispatch is never acknowledgment: Confirmed backends
    // publish accepted values through sliceChanged/panCenterBandwidthChanged,
    // including after deferred DSP adoption. Refusal/rollback leaves the last
    // observation intact. This does not grant any capability.
    virtual ReceiveControlPolicy receiveControlPolicy() const { return ReceiveControlPolicy::Optimistic; }
    virtual void setSliceFrequency(int sliceId, double hz) = 0;
    virtual void setSliceMode(int sliceId, const QString& mode) = 0;
    virtual void setSliceFilter(int sliceId, int lowHz, int highHz) = 0;
    // Select a stable radio-owned RX filter preset. The passband setter above
    // remains exclusively a resize/reposition intent; keeping the two verbs
    // distinct prevents a width that happens to equal a preset from changing
    // slots. Empty RadioCapabilities::rxFilterControl.presets means callers
    // never invoke this default no-op.
    virtual void setSliceFilterPreset(int sliceId, int presetId)
    {
        Q_UNUSED(sliceId);
        Q_UNUSED(presetId);
    }
    // Receive AGC. mode is the neutral vocabulary the slice model uses —
    // "off" / "slow" / "med" / "fast"; thresholdDb is the operator's 0..100
    // AGC-threshold value. A backend whose hardware owns the AGC translates
    // both to its wire protocol; one that owns an engine-side DSP chain
    // configures that chain. Sent as a pair because a backend configuring a DSP
    // AGC generally needs both to make either meaningful.
    virtual void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) = 0;
    // Move the panadapter's centre: the receiver's WINDOW, not the slice. A backend
    // streaming a fixed window retunes it; one owning a DDC moves the NCO.
    // Where the scope window is slaved to the VFO (networked Icom in centre mode),
    // moving it IS retuning: a Drag should retune, while a Range (centre riding along
    // with a zoom) must not walk the VFO. Backends with an independent window ignore
    // the intent.
    enum class PanCenterIntent {
        Drag,   // the operator dragged the spectrum or waterfall
        Range,  // the centre rode along with a bandwidth/zoom change
    };
    // No default, deliberately: a caller that forgot the intent would silently get
    // Range, which on a VFO-slaved window refuses the drag.
    virtual void setPanCenter(const QString& panId, double hz,
                              PanCenterIntent intent) = 0;

    // A confirmed-control backend admits the receiver and its display intent
    // together. False means neither intent was accepted; true is admission,
    // not completion. Observations still arrive only after capture adoption.
    enum class ReceiveTuneView { Preserve, Reveal, Center };
    virtual bool requestReceiveTune(int sliceId, double hz, ReceiveTuneView view)
    {
        Q_UNUSED(sliceId); Q_UNUSED(hz); Q_UNUSED(view);
        return false;
    }
    virtual bool recenterReceiveCapture(const QString& panId)
    {
        Q_UNUSED(panId);
        return false;
    }

    // Change the panadapter's SPAN. A backend owning its own DDC picks a decimation
    // rate; without this the view widens while the data stays narrow.
    // hz is a REQUEST: a backend with fixed rates snaps to the nearest one and
    // reports the result via panCenterBandwidthChanged; callers must not assume it
    // was taken. Default no-op: Flex takes `display pan set … bandwidth=` as text.
    virtual void setPanBandwidth(const QString& panId, double hz)
    {
        Q_UNUSED(panId);
        Q_UNUSED(hz);
    }

    // Receive RF gain for a panadapter, in dB, for a backend whose gain is a hardware
    // register (HL2 AD9866 LNA, 0x0a) rather than `display pan set … rfgain=` text.
    // gainDb is in the range the backend advertised via panRfGainInfoChanged; it
    // clamps rather than refuses, and reports what it took on panRfGainChanged.
    // Default no-op: Flex takes rfgain as wire text.
    virtual void setPanRfGain(const QString& panId, int gainDb)
    {
        Q_UNUSED(panId);
        Q_UNUSED(gainDb);
    }

    // The backend's own automatic receive-gain control, or nullptr when it has none;
    // see AutoRfGainControl.h. BORROWED: valid only for the duration of the call that
    // obtained it; never cache it. The nullptr default means neither a family without
    // one nor shared code needs to know which families have it.
    virtual IAutoRfGainControl* autoRfGainControl() { return nullptr; }

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
    virtual void setRadioDialLock(bool locked) { Q_UNUSED(locked); }

    // Requested panadapter frame rate, in fps. A backend computing its own spectrum
    // caps production here, at the source, so the FFT is skipped rather than computed
    // and dropped; otherwise its rate is sample rate / FFT size, which tracks zoom.
    // Default no-op: a Flex radio's own display engine paces its frames.
    virtual void setPanFrameRate(const QString& panId, int fps)
    {
        Q_UNUSED(panId);
        Q_UNUSED(fps);
    }

    // The operator's FFT-average setting (Display -> FFT AVG), 0..100:
    // 0 = no time averaging; what one step means is the backend's call.
    // Same situation as setPanFrameRate(): a backend
    // that computes its own spectrum has no radio-side display engine to
    // ask, so the setting reaches it here or not at all.
    //
    // Default no-op: a Flex radio averages on the radio, and a host-computed
    // backend that does not override this keeps its own fixed behaviour.
    virtual void setPanAverage(const QString& panId, int average)
    {
        Q_UNUSED(panId);
        Q_UNUSED(average);
    }

    // The operator's weighted-average toggle (Display -> FFT). Same routing
    // and same default as setPanAverage(); what the two states mean for a
    // host-computed spectrum is the backend's call.
    virtual void setPanWeightedAverage(const QString& panId, bool on)
    {
        Q_UNUSED(panId);
        Q_UNUSED(on);
    }

    // How many screen pixels the pan's full reported bandwidth would cover
    // -- the panel's device-pixel width, widened by any display-side crop --
    // so a backend that computes its own spectrum can return one point per
    // pixel instead of a fixed count stretched across the panel. A Flex is
    // told the same thing as `xpixels` on its own wire.
    //
    // Default no-op: a Flex radio takes xpixels on the wire, and a
    // host-computed backend that does not override this keeps its own
    // fixed point count.
    virtual void setPanPixelWidth(const QString& panId, int pixels)
    {
        Q_UNUSED(panId);
        Q_UNUSED(pixels);
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

    // ---- the RADIO's own audio output ----
    // How loud the radio plays, and whether at all; distinct from per-slice gain and
    // the client's master volume. A Flex takes these as wire commands (`mixer lineout
    // gain`). A backend that feeds the radio's codec from this host must scale the
    // samples itself: an ANAN-G2 has a speaker MUTE and no speaker volume register.
    // Percent, 0..100, matching the per-slice scale.
    virtual void setLineoutGain(int percent) { Q_UNUSED(percent); }
    virtual void setLineoutMute(bool mute) { Q_UNUSED(mute); }

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

    // ---- ordinary receive-slice lifecycle ----
    // panId is backend-owned and opaque; frequencyHz is absolute RF in Hz.
    // true means the request was accepted, not completed: confirmed state arrives via
    // sliceChanged / sliceRemoved, a later failure via sliceLifecycleFailed. false is
    // final refusal; callers never fall back to another command plane. Fixed/paired
    // receiver topologies keep the default refusal; Flex and Sim keep RadioModel's
    // command-plane adapter. A backend cancels pending work on disconnect/reconnect
    // and discards completions from retired sessions or receivers before emitting;
    // reused slice ids alone cannot identify pending work.
    virtual bool createSlice(const QString& panId, double frequencyHz)
    {
        Q_UNUSED(panId);
        Q_UNUSED(frequencyHz);
        return false;
    }
    virtual bool removeSlice(int sliceId)
    {
        Q_UNUSED(sliceId);
        return false;
    }

    // ---- panadapter lifecycle ----
    // Bring up / tear down a panadapter (and, where a pan IS a receiver, the receiver
    // behind it). true = the backend took the request; the result arrives through the
    // normal signals (panCenterBandwidthChanged + sliceChanged, or panRemoved).
    // Default false = "not mine": RadioModel creates Flex pans with `display panafall
    // create`. An add/remove verb, not setReceiverCount(n), so callers never race on n.
    virtual bool createPanadapter() { return false; }
    virtual bool removePanadapter(const QString& panId)
    {
        Q_UNUSED(panId);
        return false;
    }

    // ---- manual notch filters ----
    // A notch is a null parked at an ABSOLUTE RF frequency that stays put while the
    // operator tunes (a Flex TNF), realized in the radio or in host DSP. IDs are
    // assigned by the BACKEND and learned from notchChanged(), so createNotch() takes
    // none. A backend without notches declares capabilities().maxNotchFilters = 0
    // and the UI never offers the control.
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
    virtual void setKeying(bool key, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion = {}) = 0;
    virtual IndependentTxControl independentTxControl() const { return {}; }
    virtual bool independentTxReady() const { return false; }
    virtual void stopIndependentTx(const TxCoordinator::Operation& operation,
                                   const TxCoordinator::StopRequest& request)
    {
        Q_UNUSED(operation);
        Q_UNUSED(request);
    }

    // Trusted engine composition supplies the admitted operation for backend-
    // owned producers (e.g. a TUNE tone). Copy it when starting that producer;
    // workers must never look up whichever operation is current at delivery.
    void setTransmitContext(const TxCoordinator::Context& context) { m_transmitContext = context; }

    // A client-timed CW element. This is deliberately separate from setKeying:
    // setKeying is the transmitter/PTT envelope, while this is the carrier
    // inside that envelope. A host-modulating backend turns the element into
    // shaped IQ and may use breakIn to raise/drop PTT around it; a radio-side
    // keyer translates it to its own key-line protocol. Flex keeps using its
    // timestamped NetCW path above this seam, so the default is a no-op.
    virtual void setCwKeying(bool down, bool breakIn, int breakInDelayMs, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion = {})
    {
        Q_UNUSED(down);
        Q_UNUSED(breakIn);
        Q_UNUSED(breakInDelayMs);
        Q_UNUSED(operation);
    Q_UNUSED(completion);
    }

    // Let receive audio through WHILE TRANSMITTING (normally muted on TX). For
    // diagnostics: demodulating our own transmission checks the sideband convention
    // independently, since the panadapter agrees with the transmitter by
    // construction. See docs/HERMES.md 14.6 and 15.5. Default OFF.
    virtual void setTxAudioMonitor(bool on) { Q_UNUSED(on); }

    // The operator-facing radio MON switch and level. This is deliberately
    // separate from the diagnostic receive-during-TX gate above.
    virtual void setTxMonitor(bool on, int level)
    {
        Q_UNUSED(on);
        Q_UNUSED(level);
    }

    // Tune carrier on/off at the operator's TUNE power (percent, 0..100). Flex takes
    // "transmit tune N" as text, so FlexBackend ignores this. A backend generating
    // its own carrier needs tunePowerPercent: otherwise it can only key at the RF
    // Power level setTxPower() last pushed.
    virtual void setTune(bool on, int tunePowerPercent, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion = {})
    {
        Q_UNUSED(on);
        Q_UNUSED(tunePowerPercent);
        Q_UNUSED(operation);
    Q_UNUSED(completion);
    }

    // Transmit power as a percentage, 0..100.
    //
    // Flex takes this as a text command from TransmitModel, so FlexBackend has
    // nothing to do here. A backend that owns its own drive register (HL2)
    // implements it.
    virtual void setTxPower(int percent) { Q_UNUSED(percent); }

    // The operator's CW pitch, in Hz (TransmitModel's range: 100..6000). A
    // host-demodulating backend needs it to place its passband: the marker sits on
    // the signal and the receiver produces the pitch from a BFO (see
    // Hl2Backend::cwBfoHz()). Default no-op: a Flex takes `cw pitch` as text from
    // TransmitModel.
    virtual void setCwPitch(int hz) { Q_UNUSED(hz); }

    // Radio-resident text keyer. Unlike setCwKeying(), this hands printable
    // text to a keyer in the radio; it is the neutral seam used by CWX, CAT,
    // MIDI/controller macros and the automation bridge.
    // Empty return means accepted for delivery. A non-empty string is an
    // operator-facing rejection reason; callers must not report success when
    // the backend could not preserve the requested text.
    virtual QString sendCwText(const QString& text, const TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion = {})
    {
        Q_UNUSED(operation);
    Q_UNUSED(completion);
        Q_UNUSED(text);
        return QStringLiteral("radio has no text keyer");
    }
    virtual void abortCwText(const TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion = {}) { Q_UNUSED(operation); Q_UNUSED(completion); }
    virtual void setCwSpeed(int wpm) { Q_UNUSED(wpm); }
    virtual void setCwBreakIn(bool on) { Q_UNUSED(on); }

    // The speech processor: an enable plus a normalized level. RadioCapabilities
    // publishes whether the level is Flex's three presets (0..2) or a continuous
    // range. On an Icom the halves are separate registers (16 44 enable, 14 0E level),
    // so a backend receives both. Default no-op: Flex takes this as text from
    // TransmitModel; a host-modulating backend runs its own compressor.
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

    // The radio's own ANTENNA TUNER matching cycle; not setTune(), which emits a
    // steady carrier. `start` true begins a cycle, false bypasses; the result comes
    // back on transmitChanged's ATU fields. KEYS THE TRANSMITTER on a radio with a
    // real ATU, so it sits behind the same TX gate as every other keying intent.
    virtual void setAtu(bool start, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion = {}) { Q_UNUSED(start); Q_UNUSED(operation); Q_UNUSED(completion); }

    // RECEIVE DSP THE RADIO'S OWN FIRMWARE RUNS, the set gated by
    // capabilities().hasRadioSideDsp. Enable and level travel together so a toggle
    // never lands before the level it implies; a backend without a level ignores it.
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
    // The radio's single operator-placed notch (capabilities().hasManualNotch).
    // `position` is 0..100 across the receive passband, NOT a frequency: an IC-705
    // takes 14 0D as 0000..0255 spanning the current filter. A frequency-placed
    // backend converts here, where it knows its passband. Enable and position travel
    // together so the notch never turns on where the radio last left it.
    virtual void setSliceManualNotch(int sliceId, bool on, int position)
    {
        Q_UNUSED(sliceId); Q_UNUSED(on); Q_UNUSED(position);
    }
    virtual void setSliceSquelch(int sliceId, bool on, int level)
    {
        Q_UNUSED(sliceId); Q_UNUSED(on); Q_UNUSED(level);
    }
    // BroadcastFmReceive declares supported values; accepted state returns in
    // SliceDelta only after the receiver adopts the requested configuration.
    virtual void setSliceWfmForceMono(int sliceId, bool forceMono)
    { Q_UNUSED(sliceId); Q_UNUSED(forceMono); }
    virtual void setSliceWfmDeemphasis(int sliceId, int microseconds)
    {
        Q_UNUSED(sliceId); Q_UNUSED(microseconds);
    }

    // FM repeater controls.  These are separate radio registers on an Icom
    // (tone enable, tone frequency, duplex direction and duplex magnitude),
    // while a Flex carries the same neutral values in slice status.  Keeping
    // the four intents explicit lets a backend update only the register the
    // operator touched; the grouped helper is for memory recall, where all four
    // must be re-applied after the frequency change in a deterministic order.
    virtual void setSliceFmToneMode(int sliceId, const QString& mode)
    {
        Q_UNUSED(sliceId); Q_UNUSED(mode);
    }
    virtual void setSliceFmToneValue(int sliceId, double hz)
    {
        Q_UNUSED(sliceId); Q_UNUSED(hz);
    }
    virtual void setSliceFmToneRxValue(int sliceId, double hz)
    {
        Q_UNUSED(sliceId); Q_UNUSED(hz);
    }
    virtual void setSliceFmDtcs(int sliceId, int code, bool txReverse,
                                bool rxReverse)
    {
        Q_UNUSED(sliceId); Q_UNUSED(code); Q_UNUSED(txReverse); Q_UNUSED(rxReverse);
    }
    virtual void setSliceRepeaterOffsetDir(int sliceId, const QString& direction)
    {
        Q_UNUSED(sliceId); Q_UNUSED(direction);
    }
    virtual void setSliceFmRepeaterOffset(int sliceId, double hz)
    {
        Q_UNUSED(sliceId); Q_UNUSED(hz);
    }
    virtual void setSliceFmRepeater(int sliceId, const QString& direction,
                                    double offsetHz, const QString& toneMode,
                                    double toneHz)
    {
        // The IC-705 can clear repeater tone after a frequency change.  Memory
        // recall calls this only after tuning, and enables the tone last.
        setSliceFmRepeaterOffset(sliceId, offsetHz);
        setSliceRepeaterOffsetDir(sliceId, direction);
        setSliceFmToneValue(sliceId, toneHz);
        setSliceFmToneMode(sliceId, toneMode);
    }
    virtual bool applyMemoryRecallDetails(const MemoryRecallDetails& details)
    {
        Q_UNUSED(details.filterPreset); Q_UNUSED(details.dataMode);
        Q_UNUSED(details.rxToneHz); Q_UNUSED(details.dtcsCode);
        Q_UNUSED(details.dtcsTxReverse); Q_UNUSED(details.dtcsRxReverse);
        setSliceFmRepeater(details.sliceId, details.direction, details.offsetHz,
                           details.toneMode, details.txToneHz);
        return true;
    }

    // Request a fresh snapshot from a radio-owned memory store. Backends that
    // only push changes, or whose memories live on the host, leave this a no-op.
    virtual void refreshMemories(const QString& group) { Q_UNUSED(group); }

    // Momentary receive-on-transmit-frequency state (Icom XFC). This is
    // radio-wide selected-VFO state, not a memory/slice parameter.
    virtual void setTransmitFrequencyCheck(bool on) { Q_UNUSED(on); }

    // Receive and transmit incremental tuning, Hz relative to the VFO. Two enables and
    // one offset, the IC-705's shape (21 01 RIT, 21 02 XIT, 21 00 offset). A radio
    // without RIT does not implement these.
    virtual void setRitEnabled(bool on) { Q_UNUSED(on); }
    virtual void setXitEnabled(bool on) { Q_UNUSED(on); }
    virtual void setRitOffset(int hz) { Q_UNUSED(hz); }

    // The TRANSMIT offset. Defaults to setRitOffset() because an IC-705 has ONE shift
    // register (21 00; 21 01 / 21 02 choose whether it applies to RX, TX or both). A
    // radio with two registers (Flex rit_freq / xit_freq) overrides this.
    virtual void setXitOffset(int hz) { setRitOffset(hz); }

    // Transmit audio passband, in Hz above the carrier (Phone applet TX low/high cut).
    // Flex takes `transmit set filter_low=/filter_high=` from TransmitModel; a backend
    // owning its own modulator implements this. ALWAYS POSITIVE audio Hz on both
    // sidebands: the modulator picks the sideband, so reflecting for LSB would
    // transmit on the wrong one. Once called, the operator's passband owns the
    // modulator; a per-mode default must not overwrite it on a mode change.
    virtual void setTxFilter(int lowHz, int highHz)
    {
        Q_UNUSED(lowHz);
        Q_UNUSED(highHz);
    }

    // Microphone gain, 0..100 (Phone applet MIC). Flex takes `transmit set miclevel=`
    // from TransmitModel. A host-modulating backend that takes this owns the gain:
    // nothing else scales the mic on its behalf.
    virtual void setMicGain(int level)
    {
        Q_UNUSED(level);
    }

    // Processed transmit audio, int16 interleaved stereo at sampleRateHz, for
    // backends that modulate on the host (HL2); a Flex ignores it. Already shaped
    // (test tone, compressor, EQ) so every source reaches the air through one path.
    // `source` is ORIGIN, not treatment: it decides whose level applies (the mic
    // slider applies to Microphone and ClientLeveled, not EngineGenerated); see
    // TxAudioSource.h. No default argument: defaults on virtuals bind statically.
    virtual void submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz,
                               TxAudioSource source,
                               const TxCoordinator::Context& context)
    {
        Q_UNUSED(int16Stereo);
        Q_UNUSED(sampleRateHz);
        Q_UNUSED(source);
        Q_UNUSED(context);
    }

    // Finish a finite processed-audio stream before its caller starts the PTT
    // drain timer. Stateful converters may emit delayed tail samples here;
    // streaming/no-op backends have nothing to do.
    //
    // Returns how many milliseconds of already-submitted audio are still to be
    // PLAYED after this call returns — everything queued on the host plus
    // whatever the radio buffers before its modulator — so the caller can hold
    // PTT for exactly that long rather than a compile-time worst case. Zero
    // means "nothing is buffered on your behalf; unkey when you like".
    virtual int finishTxAudio(const TxCoordinator::Context& context) { Q_UNUSED(context); return 0; }

    // ---- diagnostics ----
    // Health/status registers this backend can report (converter overload, TX FIFO,
    // thermal, firmware, link counters). Purely for display — nothing in the app
    // makes a decision from it, hence a plain map. Synchronous: every value is cached
    // from unsolicited telemetry; a backend with worker-side values caches them on
    // this thread (see Hl2Backend). `order` lists keys in display order; a key absent
    // from `values` renders as "not reported", never as zero.
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

    // Take a BORROWED pointer to the model's offline health source. Default no-op, so
    // a family without an offline instrument never sees it. Not an ownership
    // transfer: the source outlives every backend and nothing tells a backend it has
    // gone, so the owner hands the borrow back through this setter before destroying
    // it (RadioModel::releaseOfflineHealth()). A virtual rather than a
    // concrete-backend cast (#5554 §2.8; docs/HERMES.md forbids new casts).
    virtual void setOfflineHealthSource(IOfflineHealthSource*) {}

    // What the DSP is ACTUALLY configured with, as opposed to what the model asked
    // for; the probe for model/DSP divergence, which `get_state` (model-side) cannot
    // see. One entry per chain, each with a `chain` key naming it, because chains
    // need not share a vocabulary (HL2: WDSP on RX, a phasing modulator on TX).
    // Empty by default: a backend that cannot answer reports nothing, not zeros.
    virtual QVariantList dspChains() const { return {}; }

    // State of the TRANSPORT carrying this radio's streams (healthSnapshot covers the
    // radio). Feeds the title-bar heartbeat, the status-bar Network field and Network
    // Diagnostics, which otherwise read a RadioConnection / PanadapterStream that a
    // non-Flex family does not have. A backend reports the subset it can measure.
    // `reported` false (the default) means "no transport measured": the consumer keeps
    // its existing source, so the Flex path never sees a LinkStats.
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
    // Fire-and-forget: the result arrives later via extensionResult(requestId, …) /
    // extensionError. The caller mints requestId; 0 means "no reply expected".
    virtual void invokeExtension(const QString& ns, const QString& verb,
                                 quint64 requestId, const QVariant& arg = {}) = 0;

signals:
    void independentTxStopped(const AetherSDR::TxStopEvidence& evidence);
    // ---- connection state UP ----
    void connected();
    void disconnected();
    void connectionError(const QString& reason);

    // A RADIO-CONFIGURATION problem the operator should fix that does not end the
    // session. connectionError is fatal (RadioModel always starts its reconnect timer
    // on it), so advice sent there becomes a reconnect loop. If it does not stop the
    // radio working, it belongs here.
    void configurationWarning(const QString& message);

    void capabilitiesChanged();

    // Radio-authoritative state for the momentary transmit-frequency monitor.
    void transmitFrequencyCheckChanged(bool on);
    // Radio-authoritative global dial-lock state. RadioModel fans this out to
    // every slice because a radio-global control must not look per-slice.
    void radioDialLockChanged(bool locked);

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
    // Where a slice IS a receiver, closing the receiver emits BOTH this and
    // panRemoved, or the stale SliceModel keeps counting against maxSlices().
    void sliceRemoved(int sliceId);
    // Failure of an accepted ordinary lifecycle request. operation is "create"
    // or "remove"; sliceId is -1 when creation never allocated a published ID.
    // This is diagnostic, not a state delta or a split/TX completion protocol.
    void sliceLifecycleFailed(const QString& operation, int sliceId,
                              const QString& reason);
    void meterUpdate(const QString& meterId, double value);

    // WHAT THE RECEIVE FRONT END IS DOING, for families that can observe their
    // own converter. A family that cannot never emits this, and the indicator
    // above the seam never appears -- the same shape as autoRfGainControl()
    // returning nullptr.
    //
    // RFC #5535 made this visibility a CONDITION of shipping an automatic
    // gain loop, not a nicety: a regulator with 18 dB of room and a 3-5 dB
    // knee will sometimes be wrong, and wrong-and-invisible is a radio that
    // behaves strangely. See FrontEndOverload.h.
    void frontEndOverloadChanged(const AetherSDR::FrontEndOverload& state);

    // AN ARM REQUEST ON autoRfGainControl() HAS SETTLED: `armed` is what the
    // control is now doing. Emitted after EVERY outcome of setArmed() --
    // refused (armed stays false; lastArmRefusalReason() says why), armed, and
    // disarmed -- and not for a request that changed nothing. This is how a
    // view learns about an arm it did not ask for: the connect-time restore
    // inside the backend and a bridge `pan autorfgain on` both settle without
    // passing through any GUI click, and a checkbox that only read isArmed()
    // back after its own click reported the wrong state on both (#5817).
    void autoRfGainArmSettled(bool armed);

    // Normalized transmit-status delta (aetherd RFC 2.3 — TransmitModel
    // touchpoint). Typed + compiler-checked; the backend populates only the
    // fields the wire reported (across the transmit / interlock / ATU / APD /
    // APD-sampler status planes) and RadioModel drives the TransmitModel.
    void transmitChanged(const TransmitDelta& delta);
    // An explicit radio readback confirmed the keyed state. This is distinct
    // from transmitChanged because optimistic state may already equal the
    // answer and therefore produce no delta. Backends without a separate
    // command/readback plane need not emit it.
    void keyingStateConfirmed(bool keyed);

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
    void memoryRefreshStarted(int total);
    void memoryRefreshProgress(int completed, int total);
    // All deltas for this sweep precede completion. This reports radio reads;
    // RadioModel combines it with import/save results for its UI-facing signal.
    void memoryRefreshFinished(bool success, int completed, int total);

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

    // ONE slice's demodulated RX audio, for per-slice consumers (TCI channel,
    // decoder, recorder); audioFrameReady is the mixed speaker feed. Emitted
    // PRE-mute, PRE-gain and PRE-balance, like Flex DAX: muting a slice must not stop
    // a decoder on it. Flex does not emit this; its DAX channels are per-slice.
    void sliceAudioFrameReady(int sliceId, const AetherSDR::PcmFrame& pcm);


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

    // The span limits this pan can be zoomed between, in MHz (for a DDC backend, its
    // decimation rates). Keeps the zoom clamp from exceeding the data; the GUI's
    // FlexLib model table falls through to 5.4 MHz for unknown models. A backend that
    // doesn't know never emits this and the GUI keeps its model-derived clamp.
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

    // The RF-gain range and step this pan offers. Flex learns it via `display pan
    // rfgain_info`; other backends report it here (HL2 AD9866 LNA: -12..+48 dB in
    // 1 dB steps). A backend that stays silent leaves the model's defaults.
    // `unitSuffix` is what the readout appends: " dB" for a real gain register, "%"
    // for an opaque 0..255 scale with no published dB mapping.
    void panRfGainInfoChanged(const QString& panId, int low, int high, int step,
                              const QString& unitSuffix = QStringLiteral(" dB"));

    // DISCRETE front-end stages: a preamp with named positions and a stepped
    // attenuator. `labels` names every position in order and its size is the range
    // ({"OFF", "P.AMP1", "P.AMP2"} is addressed as 0, 1, 2); EMPTY (the default)
    // hides the control. Names, not numbers: an IC-705's preamp positions have no
    // published gain and its attenuator has one 20 dB step (HF and 50 MHz only).
    // What the hardware took comes back on panPreampChanged / panAttenuatorChanged.
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
    // Normalized outlets for spectrum and audio; binary frame formats are step-4
    // work, so a backend may relay the existing in-tree frame types. No separate
    // waterfall outlet: RadioModel derives the row from spectrumFrameReady, paced by
    // the pan's waterfall rate. A backend with a genuinely separate waterfall plane
    // adds an outlet together with its consumer.
    // A producer may attach wider, same-observation RF coverage. Existing
    // producers omit it; consumers interested only in the viewport keep their
    // two-argument connection. One emission owns spectrum + paced history.
    void spectrumFrameReady(int panId, const QByteArray& frame,
                            const AetherSDR::SpectrumCoverage& coverage = AetherSDR::SpectrumCoverage());
    void audioFrameReady(const AetherSDR::PcmFrame& pcm);

protected:
    TxCoordinator::Context transmitContext() const { return m_transmitContext; }
    quint64 pcmSession() const { return m_pcmSession; }

    // Compatibility publishers for current 24 kHz backends. Call on the owner
    // thread, after rejecting obsolete worker deliveries. Native-rate producers
    // will publish their own PcmFrame without using these fixed-format adapters.
    void publishLegacyAudio(const QByteArray& pcm)
    {
        if (!m_pcmLive || !isConnected()) {
            warnAudioDropped();
            return;
        }
        if (const auto frame = m_speakerPcm.legacyStereo24(pcm)) {
            emit audioFrameReady(*frame);
        }
    }
    bool publishLegacySliceAudio(int sliceId, const QByteArray& pcm)
    {
        if (!m_pcmLive || !isConnected() || sliceId < 0) {
            warnAudioDropped();
            return false;
        }
        auto it = m_slicePcm.find(sliceId);
        if (it == m_slicePcm.end()) {
            if (m_slicePcm.size() >= PcmFrameGate::kMaxStreams) {
                return false;
            }
            auto producer = std::make_unique<PcmProducer>();
            producer->start(PcmPurpose::Slice, sliceId, {}, m_pcmSession);
            const auto frame = producer->legacyStereo24(pcm);
            if (!frame) {
                return false;
            }
            m_slicePcm.emplace(sliceId, std::move(producer));
            emit sliceAudioFrameReady(sliceId, *frame);
            return true;
        }
        if (const auto frame = it->second->legacyStereo24(pcm)) {
            emit sliceAudioFrameReady(sliceId, *frame);
            return true;
        }
        return false;
    }

private:
    TxCoordinator::Context m_transmitContext;
    // One line per session, not per frame: this fires at audio rate.
    void warnAudioDropped()
    {
        if (m_pcmDropWarned == m_pcmSession) {
            return;
        }
        m_pcmDropWarned = m_pcmSession;
        qWarning() << "IRadioBackend: dropping RX audio in session" << m_pcmSession
                   << "- no live PCM producer (live =" << m_pcmLive
                   << ", connected =" << isConnected() << ")";
    }

    bool m_pcmLive = false;
    quint64 m_pcmSession = 0;
    quint64 m_pcmDropWarned = 0;
    PcmProducer m_speakerPcm;
    std::map<int, std::unique_ptr<PcmProducer>> m_slicePcm;
};

}  // namespace AetherSDR

// Contract rule 4: every seam payload is declared here and registered in
// RadioModel's constructor, so a queued or QMetaMethod-based connection of
// linkStatsUpdated delivers rather than silently dropping.
Q_DECLARE_METATYPE(AetherSDR::IRadioBackend::LinkStats)
