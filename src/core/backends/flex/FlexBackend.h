#pragma once

#include <functional>

#include <QMap>
#include <QString>

#include "core/backends/IRadioBackend.h"

class QThread;

namespace AetherSDR {

class RadioConnection;
class PanadapterStream;

// FlexBackend — the first IRadioBackend implementor (aetherd RFC step 2),
// wrapping the SmartSDR / FlexRadio wire stack.
//
// As of 2.2b it OWNS the wire objects: the RadioConnection and PanadapterStream
// plus their two worker threads, created here in the exact order RadioModel
// used (panStream thread first, connection thread second) and torn down here in
// the exact #502 order (BlockingQueued stop → deleteLater → thread quit/wait).
// RadioModel holds non-owning pointers it obtains via connection()/panStream()
// and keeps its command/WAN orchestration and its sub-models — so the move is
// ownership-only and behavior-neutral.
//
// The canonical core verbs build the exact SmartSDR command strings and emit
// them through the model-provided command sink; they grow onto the live path as
// the touchpoint burndown converts each model (2.3).
class FlexBackend : public IRadioBackend {
    Q_OBJECT

public:
    explicit FlexBackend(QObject* parent = nullptr);
    ~FlexBackend() override;

    // The owned wire objects, on their worker threads. Non-owning to callers;
    // valid for the backend's lifetime. RadioModel grabs these post-construction
    // and its connection()/panStream() getters delegate to them.
    RadioConnection*  connection() const { return m_connection; }
    PanadapterStream* panStream()  const { return m_panStream; }

    // Where the core verbs emit their SmartSDR command strings — RadioModel's
    // existing sendCommand() funnel, so verbs reuse the one wire-write path.
    void setCommandSink(std::function<void(const QString&)> sink);
    // Primary keying verbs use an operation-fenced writer, never the generic
    // command sink. The bool distinguishes key-on from cleanup, not authority.
    // Trusted engine composition supplies the original operation at dispatch.
    void setTxCommandSink(std::function<void(const QString&, const TxCoordinator::Command&)> sink);
    // Slice verbs (setSliceFrequency/Mode/Filter) route through THIS sink, which
    // RadioModel wires to its TX-inhibit-guarded sendSliceCommand — so keeping
    // the encode's TX safety above the seam (RFC §6). Falls back to the generic
    // sink if unset. (aetherd RFC 2.3 encode template.)
    void setSliceCommandSink(std::function<void(const QString&)> sink);
    // Live model-string source for capabilities(). Call on the main thread,
    // where RadioModel lives — capabilities() is not thread-safe (no off-thread
    // caller exists yet; this documents the assumption).
    void setModelProvider(std::function<QString()> provider);

    // The capacity THIS radio declared, as opposed to what the model table
    // estimates for radios of its kind (#5594 item 3).
    //
    // Pushed down rather than read here because it arrives in the discovery
    // packet, which RadioModel owns; #5554 §2.7 moves desktop discovery onto the
    // unified source, and this becomes a backend-side read at that point.
    //
    // Either value may be <= 0, meaning "the radio did not say", which leaves
    // capabilities() on the model-table estimate. Announces a revision when a
    // value actually changes — the descriptor is what the control protocol
    // serializes, so a silent change is a client holding a stale limit.
    void setRadioReportedCapacity(int maxSlices, int maxPanadapters);

    // ---- IRadioBackend ----
    RadioCapabilities capabilities() const override;
    void connectRadio(const RadioConnectRequest& request) override;
    void disconnectRadio() override;
    bool isConnected() const override;
    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    void setPanCenter(const QString& panId, double hz,
                      PanCenterIntent intent) override;
    void createNotch(double centerHz, double widthHz) override;
    void setNotch(int notchId, const AetherSDR::NotchDelta& delta) override;
    void removeNotch(int notchId) override;
    void setNotchesEnabled(bool on) override;
    void sendSliceWaveformCommand(int sliceId, const QString& command);
    void setKeying(bool key, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion = {}) override;
    void setTune(bool on, int tunePowerPercent, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion = {}) override;
    void setAtu(bool start, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion = {}) override;
    void abortCwText(const TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion = {}) override;
    void invokeExtension(const QString& ns, const QString& verb,
                         quint64 requestId, const QVariant& arg = {}) override;

    // ---- status decode (aetherd RFC 2.3) ----
    // Decode the universal panadapter display fields (center/bandwidth) out of
    // a Flex "display pan" status kv-set and emit the normalized
    // panCenterBandwidthChanged signal. RadioModel calls this from its status
    // choke point (handlePanadapterStatus), so both live and deferred/replayed
    // status flow through it. The Flex-specific pan fields still decode in
    // PanadapterModel::applyPanStatus until they convert too.
    void decodePanCenterBandwidth(const QString& panId,
                                  const QMap<QString, QString>& kvs);
    // Decode the universal panadapter display level range (min_dbm/max_dbm) and
    // emit the normalized panRangeChanged signal. dBm is signed, so an omitted
    // field is carried as NaN ("unchanged"), not a negative sentinel.
    void decodePanRange(const QString& panId,
                        const QMap<QString, QString>& kvs);
    // Universal pan RF gain / antenna selection — decoded from Flex status and
    // emitted as the normalized typed signals (aetherd RFC 2.3; rfgain+antenna
    // promoted to universal per the 2026-07-05 classification).
    void decodePanRfGain(const QString& panId, const QMap<QString, QString>& kvs);
    void decodePanAntenna(const QString& panId, const QMap<QString, QString>& kvs);
    // Waterfall line duration (universal display timing), decoded from the
    // waterfall-status plane and emitted as panWaterfallLineDurationChanged.
    void decodeWaterfallLineDuration(const QString& panId,
                                     const QMap<QString, QString>& kvs);
    // Decode the Flex-specific pan fields that are NOT part of the core profile
    // (the WNB group) and emit them on the namespaced extensionStatus channel.
    // (aetherd RFC 2.3 extension template.)
    void decodePanExtensions(const QString& panId,
                             const QMap<QString, QString>& kvs);
    // The remaining Flex-specific display-pan status keys — wide, loop A/B, fps,
    // preamp, DAX-IQ channel, MultiFlex client_handle ownership, waterfall
    // stream-id — bundled onto the namespaced extensionStatus("flex","panState")
    // channel. Present-only (each key rides only when the wire reported it).
    void decodePanState(const QString& panId, const QMap<QString, QString>& kvs);
    // Decode the SmartSDR meter-status wire body (definitions or a "N removed"
    // line) and emit the normalized meterDefined/meterRemoved signals. Mirrors
    // FlexLib Radio.cs ParseMeterStatus. (aetherd RFC 2.3 — MeterModel touchpoint.)
    void decodeMeterStatus(const QString& rawBody);
    // Decode a Flex slice-status kv-set into the normalized, canonically-named
    // slice change map and emit sliceChanged(sliceId, changes). Owns all the
    // Flex wire knowledge (key names like "RF_frequency"/"filter_lo", "1"→bool,
    // comma-split lists); SliceModel::applyChanges consumes only canonical keys.
    // (aetherd RFC 2.3 — SliceModel touchpoint, full canonical rename.)
    void decodeSliceStatus(int sliceId, const QMap<QString, QString>& kvs);

    // Decode the five Flex transmit-family status planes into a normalized,
    // typed TransmitDelta and emit transmitChanged (aetherd RFC 2.3 — TransmitModel
    // touchpoint). Each owns its SmartSDR wire keys, "1"→bool, ok-guarded +
    // clamped numeric parses, uppercase, and list split; the model applies the
    // present fields. Called from the matching RadioModel status choke points.
    void decodeTransmitStatus(const QMap<QString, QString>& kvs);
    // Decode radio-global status ("radio …" / "radio slices …" etc.) into the
    // normalized RadioDelta and emit radioChanged (aetherd RFC 2.3 — RadioModel
    // residual). Present-only, ok-guarded numeric parses.
    void decodeRadioStatus(const QMap<QString, QString>& kvs);
    void decodeInterlockStatus(const QMap<QString, QString>& kvs);
    void decodeAtuStatus(const QMap<QString, QString>& kvs);
    void decodeApdStatus(const QMap<QString, QString>& kvs);
    // Translate a SmartSDR "amplifier <handle> …" status into a typed AmpDelta
    // and emit amplifierChanged (aetherd 2.4 — AmpModel decode split, #4094).
    // `model` is the wire "model" key (TunerGeniusXL routes to the tuner, not
    // here); `removed` marks an "amplifier <handle> removed". Stateless — the
    // presence latch / operate change-gating live in AmpModel::applyChanges.
    void decodeAmplifierStatus(const QString& handle, const QString& model,
                               const QMap<QString, QString>& kvs, bool removed);
    // Translate a SmartSDR TGXL tuner status (the "atu <handle> …" and
    // "amplifier <handle> model=TunerGeniusXL …" kv-sets) into a typed
    // TunerDelta and emit tunerChanged (aetherd 2.4 — TunerModel decode split,
    // #4092). Present-only, strict-parity with the prior TunerModel::applyStatus.
    // `handle` is the (sanitized) TGXL object handle RadioModel extracted; it is
    // cached here to source the tuner encode path (#4198).
    void decodeTunerStatus(const QString& handle, const QMap<QString, QString>& kvs);
    // Drop the cached amp/tuner encode handles (#4198). Called on radio
    // disconnect/reset, where RadioModel clears the model-side presence outside
    // the decode path — without this the stale handle could survive a reconnect
    // to a *different* radio and be used to build a bogus relay command.
    void clearExtensionHandles();
    void decodeApdSamplerStatus(const QMap<QString, QString>& kvs);
    // Decode the Flex GPS-status line ("gps …", '#'-separated key=value tokens)
    // into a present-only GpsDelta and emit gpsChanged (aetherd RFC 2.3 —
    // RadioModel residual). Satellite counts are numeric; the rest keep the
    // radio's string form (units included).
    void decodeGpsStatus(const QString& rawBody);
    // Decode a Flex memory-slot status kv-set (keyed by slot index) into a
    // normalized MemoryDelta and emit memoryChanged (aetherd RFC 2.3 — RadioModel
    // residual). Sets delta.removed for "in_use=0" / a bare "removed"; carries
    // text fields raw (the model sanitises). Present-only, ok-guarded numerics.
    void decodeMemoryStatus(int index, const QMap<QString, QString>& kvs);
    // Parse a Flex "profile <type> …" status body into a ProfileDelta and emit
    // profileChanged (aetherd RFC 2.3 — RadioModel residual). Handles the raw
    // ('^'-list, space-containing values) list/current form and the plain
    // importing/exporting flags. profileType is "tx"/"mic"/"global"; empty for a
    // flags-only kv-set with no profile type.
    void decodeProfileStatus(const QString& profileType, const QString& rawBody);
    void decodeProfileFlags(const QMap<QString, QString>& kvs);

private:
    void send(const QString& cmd);
    void sendTx(const QString& cmd, const TxCoordinator::Command& command);
    void sendSlice(const QString& cmd);   // guarded slice path (§6)

    RadioConnection*  m_connection{nullptr};    // owned; lives on m_connThread
    QThread*          m_connThread{nullptr};    // owned (this-parented)
    PanadapterStream* m_panStream{nullptr};     // owned; lives on m_networkThread
    QThread*          m_networkThread{nullptr}; // owned (this-parented)
    std::function<void(const QString&)> m_sink;
    std::function<void(const QString&, const TxCoordinator::Command&)> m_txSink;
    std::function<void(const QString&)> m_sliceSink;
    std::function<QString()> m_modelProvider;

    // Decode-side handle state (#4198). Captured from the amplifier/tgxl status
    // decode and consumed by invokeExtension() to build the amp/tuner relay wire,
    // so RadioModel no longer threads the Flex handle through the encode arg.
    // Touched only on the main thread — both the decode (RadioModel::handleStatus)
    // and the encode intent lambdas run there — so a plain QString needs no sync.
    QString m_ampHandle;
    QString m_tunerHandle;

    // Radio-declared capacity (#5594 item 3), 0 until the radio says. Cleared by
    // clearExtensionHandles() on disconnect so a different radio cannot inherit
    // the previous one's limits.
    int m_reportedMaxSlices{0};
    int m_reportedMaxPanadapters{0};

    // The model name the last capabilitiesChanged() announcement described
    // (#5594, M1). Flex's whole capability table is derived from the model name
    // — capabilitiesFor(caps.model) seeds maxSlices, the DSP tier and the rest —
    // and that name arrives in a `radio ...` status AFTER the connect edge, so
    // without this the descriptor silently changed with nothing announcing it.
    // Held here rather than compared through m_modelProvider so the guard does
    // not depend on radioChanged being delivered synchronously.
    // Cleared by clearExtensionHandles() on disconnect: a reconnect to a
    // DIFFERENT radio must announce again.
    QString m_announcedModel;
};

}  // namespace AetherSDR
