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
#include "core/backends/ProfileDelta.h"
#include "core/backends/RadioCapabilities.h"
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

    // ---- connection lifecycle ----
    virtual void connectRadio(const RadioConnectRequest& request) = 0;
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
    virtual void setPanCenter(const QString& panId, double hz) = 0;

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

    // TX keying intent. The decision to allow keying is made ABOVE this seam by
    // the engine guard (RFC §6, single-holder lock + capability check); the
    // backend only translates an already-authorized intent to its mechanism
    // (command verb, in-stream bit, hardware line). A backend whose
    // capabilities().canTransmit is false implements this as a no-op.
    virtual void setKeying(bool key) = 0;

    // Tune carrier on/off.
    //
    // Flex takes "transmit tune N" as a text command, so FlexBackend has nothing
    // to do here. A backend that generates its own carrier implements it.
    virtual void setTune(bool on) { Q_UNUSED(on); }

    // Transmit power as a percentage, 0..100.
    //
    // Flex takes this as a text command from TransmitModel, so FlexBackend has
    // nothing to do here. A backend that owns its own drive register (HL2)
    // implements it.
    virtual void setTxPower(int percent) { Q_UNUSED(percent); }

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
    virtual void submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz)
    {
        Q_UNUSED(int16Stereo);
        Q_UNUSED(sampleRateHz);
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
    void capabilitiesChanged();

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
    void panRfGainInfoChanged(const QString& panId, int low, int high, int step);

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
