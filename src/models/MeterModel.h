#pragma once

#include <QObject>
#include <QMap>
#include <QJsonArray>
#include <QString>

#include <optional>

// MeterDef moved to the core backend layer (aetherd RFC 2.3 / #4070) so the
// vendor-neutral IRadioBackend can carry it on meterDefined() without a
// model dependency. Re-exported here for the model's existing users.
#include "core/backends/MeterDef.h"

namespace AetherSDR {

// Central meter value store.
//
// The radio defines meters via TCP status messages (parsed by RadioModel)
// and streams real-time values via VITA-49 PCC 0x8002 UDP packets.
//
// VITA-49 meter payload: N pairs of (uint16 meter_id, int16 raw_value).
// Conversion (from FlexLib Meter.cs UpdateValue):
//   dBm/dB/dBFS/SWR → raw / 128.0
//   Volts/Amps       → raw / 256.0 (firmware >= 1.11; 1024 for older)
//   degF/degC        → raw / 64.0
//   default          → raw (no scaling)
class MeterModel : public QObject {
    Q_OBJECT

public:
    explicit MeterModel(QObject* parent = nullptr);

    // Set the TGXL amplifier handle so AMP meters can be routed correctly
    // (TGXL FWD/RL → TunerApplet, PGXL FWD/RL → AmpApplet)
    void setTgxlHandle(quint32 handle);

    // Register or update a meter definition from a TCP status message.
    void defineMeter(const MeterDef& def);

    // Remove a meter by index.
    void removeMeter(int index);

    // Clear all meter definitions and cached values on disconnect/reconnect.
    void clear();

    // Track which radio slice currently owns TX. Compression TX-chain
    // COMPPEAK meters are repeated per slice, so MeterModel must resolve
    // indexes against this slice instead of using last-match-wins globals.
    void setActiveTxSlice(int sliceIndex);

    // Process a batch of raw meter values from a VITA-49 packet.
    // ids[i] is the meter index, vals[i] is the raw int16 value.
    void updateValues(const QVector<quint16>& ids, const QVector<qint16>& vals);

    // Set one meter from an ALREADY-CONVERTED value, addressed by source+name.
    //
    // For backends that decode their own telemetry (HL2) rather than streaming
    // Flex's raw meter packets. It converts back to the raw fixed-point form and
    // goes through updateValues() on purpose: every derived quantity — forward
    // power smoothing, SWR, TX-meter freshness timestamps, the change signals —
    // lives in that path, and a second entry point that recomputed any of it
    // would drift from the first.
    //
    // Returns false if no such meter is defined.
    bool updateValueByName(const QString& source, const QString& name,
                           float converted, int sourceIndex = -1);

    // Lookup a meter definition by index.
    const MeterDef* meterDef(int index) const;

    // Find the meter index for a given source+name (e.g. "SLC", "LEVEL").
    int findMeter(const QString& source, const QString& name, int sourceIndex = -1) const;

    // Current converted value for a meter index. Returns 0 if unknown.
    float value(int index) const;

    // Snapshot helpers for diagnostics and issue reporting.
    QJsonArray allMeters() const;
    QJsonArray metersForSource(const QString& source, int sourceIndex = -1) const;

    // Convenience: S-meter (slice LEVEL meter) in dBm.
    float sLevel() const { return m_sLevel; }

    // Convenience: forward power in watts.
    float fwdPower() const { return m_fwdPower; }
    float fwdPowerInstant() const { return m_fwdPowerInstant; }
    float reflectedPower() const { return m_reflectedPower; }
    float tgxlFwdPower() const { return m_tgxlFwdPwr; }

    // How long after the last TX meter sample a TX-derived reading still counts
    // as live. Matches the window AutomationServer already uses to publish
    // `txMetersFresh` alongside these values, so the flag and the values it
    // describes cannot disagree. (RigctlProtocol uses a tighter 1500 ms and
    // additionally requires an active transmit — a stricter gate, not a
    // conflicting one.)
    static constexpr qint64 kTxMeterStaleMs = 2000;

    // Convenience: SWR.
    // ⚠ May be a stale reading from a previous transmit. Callers that display
    // it must gate on swrIfLive(); `allMeters()` and `metersForSource()`
    // already gate their SWR entry the same way.
    float swr() const { return m_swr; }
    // SWR when the SWR SAMPLE ITSELF is live, std::nullopt when it is not.
    // Judged by SWR's own timestamp, not the aggregate TX stamp: a radio that
    // keeps streaming power but stops streaming SWR must not report a
    // minutes-old ratio as current (#4533; #4536 review). This is the one
    // definition of "no valid SWR" — the signals' swrValid flag, both
    // snapshot arrays and the bridge scalar all derive from the same
    // timestamp and constant.
    std::optional<float> swrIfLive() const;
    // THE one liveness predicate behind swrIfLive(), the signals' swrValid
    // flag and both snapshot arrays. SWR is live when its own sample is
    // within swrMaxAgeMs AND, on a backend that actually publishes forward
    // power, that power is fresh and above the qualifying floor. A backend
    // that has never published FWDPWR (the HL2 — uncalibrated counts, its
    // SWR stream is already power-gated at the source) is judged on the SWR
    // sample alone, by construction rather than by exception.
    bool swrSampleLive(qint64 nowMs, qint64 swrMaxAgeMs) const;
    float tgxlSwr() const { return m_tgxlSwr; }

    // Timestamp of the last TX meter sample (milliseconds since epoch).
    qint64 txMetersUpdatedAtMs() const { return m_lastTxMeterUpdateMs; }
    qint64 fwdPowerUpdatedAtMs() const { return m_lastFwdPowerUpdateMs; }
    qint64 reflectedPowerUpdatedAtMs() const { return m_lastReflectedPowerUpdateMs; }
    qint64 swrUpdatedAtMs() const { return m_lastSwrUpdateMs; }
    qint64 tgxlFwdPowerUpdatedAtMs() const { return m_lastTgxlFwdPowerUpdateMs; }
    qint64 tgxlSwrUpdatedAtMs() const { return m_lastTgxlSwrUpdateMs; }
    bool hasRecentTxMeters(qint64 maxAgeMs) const;
    bool hasRecentReflectedPower(qint64 maxAgeMs) const;

    // Test seam: age the TX-meter timestamp so staleness behaviour can be
    // exercised without sleeping through the real window.
    void setLastTxMeterUpdateMsForTest(qint64 ms) { m_lastTxMeterUpdateMs = ms; }
    // SWR is gated on ITS OWN timestamp (#4536), so staleness tests must age
    // this one; ageing only the aggregate stamp exercises nothing the SWR
    // gate reads.
    void setLastSwrUpdateMsForTest(qint64 ms) { m_lastSwrUpdateMs = ms; }

    // Convenience: mic peak level (dBFS) and radio-provided compression (dB).
    float micPeak()  const { return m_micPeak; }
    float compPeak() const { return m_compPeak; }
    bool hasCompressionMeterValue() const { return m_hasCompPeakValue; }

    // Convenience: instantaneous mic level and compression (non-peak).
    float micLevel() const { return m_micLevel; }
    float compLevel() const { return m_compLevel; }

    // Convenience: external Hardware ALC RCA jack voltage (dBFS, from TX
    // "HWALC" meter).  Permanently zero unless an external Hardware ALC
    // connection is wired into the radio's HWALC RCA — kept around for
    // SliceTroubleshootingDialog telemetry; not what users normally watch.
    float hwAlc() const { return m_hwAlc; }
    // Convenience: post-software-ALC SSB-peak meter (dBFS, from TX "ALC").
    // This is the indicator users actually want — moves with voice peaks
    // and CW keying envelope.  Drives the ALC gauges in the Phone/CW applet.
    float swAlc() const { return m_swAlc; }

    // Convenience: PA heatsink temperature (°C).
    float paTemp() const { return m_paTemp; }

    // Convenience: supply voltage (Volts, from "+13.8A" meter — measurement point A, before fuse).
    float supplyVolts() const { return m_supplyVolts; }
    // Whether a supply-voltage SAMPLE has actually arrived — not merely
    // whether the radio declared the meter. The distinction is load-bearing:
    // m_supplyIdx is set when the meter DEFINITION lands, while m_supplyVolts
    // stays at its 0.0f initialiser until a "+13.8A" VALUE packet lands, and
    // hwTelemetryChanged fires on every PA-temperature tick in between because
    // one signal reports both halves. Keying on the index would therefore let
    // a PATEMP tick in that window repaint the initialiser as "0.00 V" —
    // exactly the fabricated reading this accessor exists to prevent. Same
    // shape as m_hasCompPeakValue above. Lets a caller tell "the rail reads
    // zero" from "no rail reading has arrived".
    bool hasSupplyVoltage() const { return m_hasSupplyVoltsValue; }

signals:
    // Emitted when the S-meter value changes (dBm).
    // sliceIndex identifies which slice's LEVEL meter this is.
    void sLevelChanged(int sliceIndex, float dbm);

    // Emitted when the ESC meter value changes (signal strength after ESC, dBm).
    void escLevelChanged(int sliceIndex, float dbm);

    // Emitted when TX meters change (power, SWR).
    //
    // THE SWR-ABSENT CONTRACT (one definition for every surface — #4536):
    // swrValid=false means "no current SWR measurement exists". The float
    // carried alongside is 0.0f and MUST NOT be interpreted — not clamped to
    // 1.0, not treated as the radio's <1.0 over-range sentinel, not rendered.
    // Absence is a distinct state, exactly as the snapshot path reports
    // has_value=false / value=null / age=-1. Whose timestamp governs: SWR'S
    // OWN (swrUpdatedAtMs()), not FWDPWR's and not the aggregate TX stamp — a
    // reading is absent when the SWR sample itself is old, regardless of what
    // other meters are doing.
    void txMetersChanged(float fwdPower, float swr, bool swrValid);

    // Independent directional-coupler readings for a physical cross-needle
    // display. Forward power is the raw FWDPWR sample so both movements receive
    // one layer of identical GUI ballistics. reflectedPowerMeasured is false
    // when REFPWR is unavailable/stale and the consumer must derive it from SWR.
    // swrValid: see txMetersChanged — same contract, same timestamp.
    void directionalPowerMetersChanged(float forwardPower,
                                       float reflectedPower,
                                       float swr,
                                       bool swrValid,
                                       bool reflectedPowerMeasured);

    // Emitted on every FWDPWR sample with the raw pre-smoothed value (watts).
    // Consumers (e.g. TxApplet's PEP peak-hold tick) want the instant peak
    // information that the double-smoothed m_fwdPower in txMetersChanged
    // attenuates by 1-2 dB on SSB. (#2561)
    void txPeakChanged(float fwdPowerInstant);

    // Emitted when mic meters change (instantaneous level, compression,
    // and peak values for peak-hold markers).
    void micMetersChanged(float micLevel, float compLevel,
                          float micPeak, float compPeak);

    // Emitted when the external Hardware ALC RCA voltage changes (dBFS).
    void hwAlcChanged(float dbfs);
    // Emitted when the post-software-ALC SSB-peak meter changes (dBFS).
    // Drives the in-app ALC gauges; fires during voice peaks and CW keying.
    void swAlcChanged(float dbfs);

    // Emitted when hardware telemetry meters change (PA temp, supply voltage).
    void hwTelemetryChanged(float paTemp, float supplyVolts);

    // Emitted when amplifier meters change (PGXL fwd power, SWR, temp).
    void ampMetersChanged(float fwdPower, float swr, float temp);
    void tgxlMetersChanged(float fwdPower, float swr);

    // Emitted when any meter value changes (for debug/generic display).
    void meterUpdated(int index, float value);

private:
    float convertRaw(const MeterDef& def, qint16 raw) const;
    void clearCompressionState();
    void recomputeSourceIndexMins();
    bool isTxWaveformMeter(const MeterDef& def) const;
    bool hasExplicitTxWaveformSourceIndex(const MeterDef& def) const;
    int implicitTxWaveformSliceIndex() const;
    int txWaveformBase() const;
    int activeTxWaveformSourceIndex() const;
    int compPeakIndexForActiveTxSlice() const;
    void logCompressionMeterMap(const MeterDef& def) const;
    void logCompressionSummary(const char* reason, bool force = false);

    QMap<int, MeterDef> m_defs;        // meter index → definition
    QMap<int, float>    m_values;      // meter index → last converted value
    QMap<int, qint64>   m_valueUpdatedMs; // meter index → epoch ms of last value
                                          // update. Lets consumers reject stale
                                          // reads (e.g. PACURRENT, which the
                                          // radio sends only ~1 s into TX). (#3646)

    // Cached indices for fast lookup of important meters
    QMap<int, int> m_sLevelIdxBySlice;  // sliceIndex → meter index for "SLC"/"LEVEL"
    QMap<int, int> m_escLevelIdxBySlice; // sliceIndex → meter index for "SLC"/"ESC"
    QMap<int, int> m_compPeakIdxByTxSource; // TX waveform sourceIndex → "COMPPEAK"
    QMap<int, int> m_compPeakIdxBySlice;    // active slice → "COMPPEAK" for TX blocks with num=0
    int m_minSliceSourceIndex{-1};
    int m_minTxWaveformSourceIndex{-1};
    int m_manifestSliceContext{-1};
    int m_activeTxSlice{-1};
    int m_fwdPwrIdx{-1};     // "FWDPWR"
    int m_refPwrIdx{-1};     // "REFPWR"
    int m_swrIdx{-1};        // "SWR"
    int m_micPeakIdx{-1};    // "COD-" / "MICPEAK" (hardware mic)
    int m_micLevelIdx{-1};   // "COD-" / "MIC" (hardware mic RX level)
    int m_compLevelIdx{-1};  // "TX" / "COMP" (instantaneous)
    int m_hwAlcIdx{-1};      // "TX" / "HWALC" — external RCA jack voltage
    int m_swAlcIdx{-1};      // "TX" / "ALC"   — post-software-ALC SSB peak
    int m_paTempIdx{-1};     // "RAD" / "PATEMP"
    int m_supplyIdx{-1};     // "RAD" / "+13.8A" (supply voltage, point A = before fuse)
    int m_ampFwdPwrIdx{-1};  // "AMP" / "FWD" (PGXL)
    int m_ampSwrIdx{-1};     // "AMP" / "RL" (PGXL)
    int m_ampTempIdx{-1};    // "AMP" / "TEMP"
    int m_tgxlFwdIdx{-1};   // "AMP" / "FWD" (TGXL — matched by handle)
    int m_tgxlSwrIdx{-1};   // "AMP" / "RL" (TGXL — matched by handle)
    quint32 m_tgxlHandle{0}; // TGXL amplifier handle for meter disambiguation
    float m_tgxlFwdPwr{0.0f};
    float m_tgxlSwr{1.0f};
    qint64 m_lastTgxlFwdPowerUpdateMs{0};
    qint64 m_lastTgxlSwrUpdateMs{0};

    // Cached values
    float m_sLevel{-130.0f};
    float m_fwdPower{0.0f};
    float m_fwdPowerInstant{0.0f};
    float m_reflectedPower{0.0f};
    float m_swr{1.0f};
    qint64 m_lastTxMeterUpdateMs{0};
    qint64 m_lastFwdPowerUpdateMs{0};
    qint64 m_lastReflectedPowerUpdateMs{0};
    qint64 m_lastSwrUpdateMs{0};
    float m_micPeak{-50.0f};
    float m_compPeak{0.0f};       // radio-provided compression amount, 0..25 dB
    bool m_hasCompPeakValue{false};
    float m_compPeakLevel{0.0f};  // last raw converted COMPPEAK sample
    bool m_hasCompPeakLevel{false};
    qint64 m_compPeakUpdatedMs{0};
    qint64 m_lastCompressionSummaryLogMs{0};
    QString m_lastCompressionSummaryReason;
    float m_micLevel{-50.0f};
    float m_compLevel{0.0f};
    float m_hwAlc{0.0f};
    float m_swAlc{0.0f};
    float m_paTemp{0.0f};
    float m_supplyVolts{0.0f};
    // Set when a "+13.8A" value packet lands, cleared wherever m_supplyIdx is,
    // so it can never outlive the meter it describes. See hasSupplyVoltage().
    bool m_hasSupplyVoltsValue{false};
    float m_ampFwdPwr{0.0f};
    float m_ampSwr{1.0f};
    float m_ampTemp{0.0f};
};

} // namespace AetherSDR
