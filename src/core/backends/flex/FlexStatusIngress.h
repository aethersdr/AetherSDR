#pragma once

#include "CommandParser.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/CwxStatusDelta.h"
#include "core/backends/InterlockStatusDelta.h"
#include "core/backends/SpotStatusDelta.h"
#include "core/backends/StreamStatusDelta.h"
#include "core/backends/WaveformStatusDelta.h"
#include "core/backends/TxBandDelta.h"

#include <QMetaObject>
#include <QObject>
#include <QPointer>

namespace AetherSDR {

class RadioConnection;
class WanConnection;

// The one entry for Flex wire lines. Each transport (the LAN RadioConnection,
// a SmartLink WanConnection, the synthetic Demo connection) emits every line
// once as lineReceived(line, epoch), tagged with the status epoch it was
// stamped with when its session began. The ingress lives on its owner's
// thread and takes each line over one queued hop, accepting it only from the
// current route and the current epoch: a line from an abandoned attempt, a
// retired session or a replaced transport never reaches the model, however
// it was queued.
class FlexStatusIngress final : public QObject {
    Q_OBJECT
public:
    explicit FlexStatusIngress(QObject* parent = nullptr);
    // Route a LAN (or synthetic) connection; its next session is stamped with
    // beginSession()'s epoch before it connects.
    void bind(RadioConnection* connection);
    // Route a SmartLink session that is already live (it binds after its
    // handshake): stamps and returns a fresh epoch.
    quint64 bindLive(WanConnection* connection);
    void unbind();
    // A fresh epoch for the session about to start; every line queued so far
    // is refused. The caller stamps the transport with it before connecting.
    quint64 beginSession();
    // Refuse the given epoch from now on, if it is still the current one.
    void retire(quint64 epoch);
    quint64 epoch() const { return m_epoch; }
    // Off: accepted lines skip the value decoders and only reach lineAccepted
    // (SimBackend, whose native producer owns those values).
    void setDecodeValues(bool on) { m_decodeValues = on; }
    // "memory N [words…]" → the slot index and its key/values, with trailing
    // bare words folded in; nullopt when the index does not parse.
    static std::optional<std::pair<int, QMap<QString, QString>>>
    memoryStatus(const QString& object, const QMap<QString, QString>& kvs);
    // "file update …" → the radio's firmware-install report; nullopt for any
    // other object.
    static std::optional<FirmwareUpdateStatus>
    fileUpdateStatus(const QString& object, const QMap<QString, QString>& kvs);
    // "stream <id> …" / "tx_audio_stream <id> …" → the model's stream rules'
    // view of it; nullopt for any other object.
    static std::optional<StreamStatusDelta>
    streamStatus(const QString& object, const QMap<QString, QString>& kvs);

    // A line as an accepted one is handled (decoders, then lineAccepted),
    // without the fence: RadioModel::handleLineForTest().
    void deliverForTest(const ParsedMessage& line)
    {
        const QPointer<FlexStatusIngress> alive(this);
        decodeLine(line);
        if (alive) { emit lineAccepted(line); }
    }
    void decodeForTest(const ParsedMessage& line) { decodeLine(line); }

    // ---- Decoders (FlexStatusDecoders.cpp) ----
    // Every accepted line is decoded before lineAccepted carries it on, so the
    // typed deltas land ahead of the model's own handling of the same line.
    // Public so a test can feed one body; production feeds whole lines.
    //
    // The transmit-family planes (transmit, atu, apd, apd sampler) → typed
    // TransmitDelta: SmartSDR wire keys, "1"→bool, ok-guarded and clamped
    // numerics, uppercase, list split; the model applies the present fields.
    void decodeTransmitStatus(const QMap<QString, QString>& kvs);
    void decodeAtuStatus(const QMap<QString, QString>& kvs);
    void decodeApdStatus(const QMap<QString, QString>& kvs);
    void decodeApdSamplerStatus(const QMap<QString, QString>& kvs);
    // The meter-status body (definitions or "N removed"), as FlexLib Radio.cs
    // ParseMeterStatus.
    void decodeMeterStatus(const QString& rawBody);
    // The GPS line's '#'-separated tokens → present-only GpsDelta. Satellite
    // counts are numeric; the rest keep the radio's string form.
    void decodeGpsStatus(const QString& rawBody);
    // A raw body (after "S<handle>|") whose object is read from the raw line
    // itself; false when it is not one of them. Today: ntp_sources, whose
    // quoted description keeps its spaces only there.
    bool decodeRawStatus(const QString& body);
    // `ntp_sources id=<id> …` → present-only NtpSourceDelta (FlexLib
    // NtpSource.cs SplitStatusTokens for the quoted description).
    void decodeNtpSourceStatus(const QString& rawBody);
    // A memory-slot kv-set → MemoryDelta (removed for "in_use=0" or a bare
    // "removed"; text raw, the model sanitises).
    void decodeMemoryStatus(int index, const QMap<QString, QString>& kvs);
    // "profile <type> …" ('^' lists, values with spaces, current, flags), and
    // the space-free flags of an untyped profile object.
    void decodeProfileStatus(const QString& profileType, const QString& rawBody);
    void decodeProfileFlags(const QMap<QString, QString>& kvs);
    // Stream lifecycle ("stream 0x…", legacy "tx_audio_stream …"): audio
    // stream deltas, DAX TX ownership, and NetCW removal (cwStreamRemoved).
    void decodeStreamStatus(const QString& object, const QMap<QString, QString>& kvs);
    void decodeAudioStreamStatus(const QString& object, const QMap<QString, QString>& kvs);
    void decodeDaxTxStatus(const QString& object, const QMap<QString, QString>& kvs);
    // "transmit band N …" → the band's transmit plane.
    void decodeTxBandStatus(int bandId, const QMap<QString, QString>& kvs);
    // "interlock band N …" → the band's interlock plane.
    void decodeInterlockBandStatus(int bandId, const QMap<QString, QString>& kvs);
    // "interlock band N tune_tx_suppress=…" → whether TUNE keys that band's
    // outputs (tuneOutputSuppressionObserved); the bands seen so far gate
    // FlexBackend's TUNE-output edits.
    void decodeTuneOutputStatus(const QString& object, const QMap<QString, QString>& kvs);
    // "tnf N …" → a notch (absolute Hz) or its removal: the seam's notch signals.
    void decodeTnfStatus(int notchId, bool removed, const QMap<QString, QString>& kvs);
    // "amplifier <handle> …" → a power amplifier (AmpDelta) or the TGXL tuner
    // (TunerDelta); FlexLib tells them apart by model=TunerGeniusXL. `removed`
    // marks an "amplifier <handle> removed". The handles each names are kept
    // for FlexBackend's encode path (#4198).
    void decodeAmplifierStatus(const QString& handle, const QString& model,
                               const QMap<QString, QString>& kvs, bool removed);
    void decodeTunerStatus(const QString& handle, const QMap<QString, QString>& kvs);
    // "interlock …" → InterlockStatusDelta; its timing fields alone are
    // interlockTiming().
    static InterlockStatusDelta interlockStatus(const QMap<QString, QString>& kvs);
    static TransmitDelta interlockTiming(const QMap<QString, QString>& kvs);
    // The tuner fields of a TGXL status, without naming its handle as the tuner.
    static TunerDelta tunerStatus(const QString& handle, const QMap<QString, QString>& kvs);
    const QString& ampHandle() const { return m_ampHandle; }
    const QString& tunerHandle() const { return m_tunerHandle; }
    void clearExtensionHandles() { m_ampHandle.clear(); m_tunerHandle.clear(); }

    bool tuneOutputBandKnown(int bandId) const { return m_tuneOutputKnownBands.contains(bandId); }
    void clearTuneOutputBands() { m_tuneOutputKnownBands.clear(); }

    // The DAX TX streams the radio reported, as decodeDaxTxStatus keeps them.
    // FlexBackend's ownership requests read them; a stream's incarnation
    // changes whenever its owner or TX state does.
    struct DaxTxStreamState {
        std::optional<quint32> clientHandle;
        std::optional<bool> tx;
        quint64 incarnation{0};
    };
    const DaxTxStreamState* daxTxStream(quint32 streamId) const
    {
        const auto it = m_daxTxStreams.constFind(streamId);
        return it == m_daxTxStreams.cend() ? nullptr : &it.value();
    }
    void clearDaxTxStreams() { m_daxTxStreams.clear(); }

signals:
    void lineAccepted(const AetherSDR::ParsedMessage& line);
    // The typed results, re-emitted by the owning backend as its own.
    void transmitChanged(const AetherSDR::TransmitDelta& delta);
    void meterDefined(const AetherSDR::MeterDef& def);
    void meterRemoved(int index);
    void gpsChanged(const AetherSDR::GpsDelta& delta);
    void ntpSourceChanged(const AetherSDR::NtpSourceDelta& delta);
    void memoryChanged(const AetherSDR::MemoryDelta& delta);
    void profileChanged(const AetherSDR::ProfileDelta& delta);
    void audioStreamChanged(const AetherSDR::AudioStreamDelta& delta);
    void daxTxStreamChanged(const AetherSDR::DaxTxStreamDelta& delta);
    // The radio removed a NetCW stream (FlexCwControls retires it).
    void cwStreamRemoved(quint32 streamId);
    void fileUpdateReported(const AetherSDR::FirmwareUpdateStatus& status);
    void txBandChanged(const AetherSDR::TxBandDelta& delta);
    void streamStatusObserved(const AetherSDR::StreamStatusDelta& delta);
    void notchChanged(int notchId, const AetherSDR::NotchDelta& delta);
    void notchRemoved(int notchId);
    void spotStatusObserved(const AetherSDR::SpotStatusDelta& delta);
    void cwxStatusObserved(const AetherSDR::CwxStatusDelta& delta);
    void graphicEqStatusObserved(const AetherSDR::GraphicEqStatus& status);
    void waveformStatusObserved(const AetherSDR::WaveformStatusDelta& status);
    void amplifierChanged(const AetherSDR::AmpDelta& delta);
    void tunerChanged(const AetherSDR::TunerDelta& delta);
    void unaddressedTunerStatus(const AetherSDR::TunerDelta& delta);
    void interlockStatusObserved(const AetherSDR::InterlockStatusDelta& delta);
    void tuneOutputSuppressionObserved(int bandId, bool suppressed);

private:
    template <typename Transport> void route(Transport* transport);
    void decodeLine(const ParsedMessage& line);
    QMetaObject::Connection m_connection;
    quint64 m_route{0};    // the current route's token; 0 = none
    quint64 m_routes{0};
    quint64 m_epoch{0};
    bool m_decodeValues{true};
    QMap<quint32, DaxTxStreamState> m_daxTxStreams;
    quint64 m_daxTxIncarnation{0};
    QSet<int> m_tuneOutputKnownBands;
    QString m_ampHandle;
    QString m_tunerHandle;
    void routeAmplifierStatus(const QString& object, const QMap<QString, QString>& kvs);
};

} // namespace AetherSDR
