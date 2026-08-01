#include "MeterModel.h"

#include <algorithm>
#include "core/LogManager.h"
#include <QDebug>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <cmath>

namespace AetherSDR {

namespace {

// Forward power at or below which the radio is treated as not transmitting, so
// the display snaps to zero instead of decaying towards it (#4540).
//
// A radio with no carrier reports 0 dBm on FWDPWR, and 10^(0/10)/1000 is
// 0.001 W — small, but NOT zero, which is the whole problem: an
// exponential-decay filter converges on it rather than reaching it. The
// threshold is a hair above that floor so the "no carrier" case is caught
// exactly, while any genuine reading (0 dBm is already 30 dB below a 1 W
// carrier) stays on the smoothed path.
constexpr float kNoCarrierWatts = 0.0011f;
// Minimum instantaneous forward power for an SWR ratio to mean anything.
//
// A radio with no carrier reports 0 dBm on FWDPWR, which is 10^(0/10)/1000 =
// 0.001 W — small but not zero. SWR is computed from forward and reflected
// power, so below this there is no power behind the ratio and it saturates:
// an HL2 published 255.99 and held it. The threshold sits a hair above that
// floor, and 0 dBm is already 30 dB below a 1 W carrier, so nothing real
// lives underneath it. (#4533)
constexpr float kMinForwardWattsForSwr = 0.0011f;
// NOTE: kNoCarrierWatts (#4540) and kMinForwardWattsForSwr (#4533) share the
// same numeric floor but answer different questions -- "is a carrier present"
// versus "is there power behind this ratio" -- and are kept separate so a
// future change to one cannot silently move the other.

constexpr qint64 kCompressionSummaryLogIntervalMs = 500;
constexpr qint64 kDirectionalMeterFreshnessMs = 500;
constexpr int kMinTxWaveformSourceIndex = 8;

float compressionValueForGauge(float compPeakDb)
{
    // The radio reports COMPPEAK directly as the compression amount.
    // Keep the model value positive (0 = none, 25 = heavy) and let widgets
    // adapt it to their visible meter face.
    return qBound(0.0f, compPeakDb, 25.0f);
}

QJsonObject meterToJson(const MeterDef& def, bool hasValue, float value, qint64 ageMs)
{
    QJsonObject obj;
    obj["index"] = def.index;
    obj["source"] = def.source;
    obj["source_index"] = def.sourceIndex;
    obj["name"] = def.name;
    obj["unit"] = def.unit;
    obj["low"] = def.low;
    obj["high"] = def.high;
    obj["description"] = def.description;
    obj["has_value"] = hasValue;
    obj["value"] = hasValue ? QJsonValue(value) : QJsonValue();
    // Milliseconds since this meter's value last updated (-1 = never). Lets a
    // consumer reject stale reads — a value with a large/-1 age is not a live
    // measurement (e.g. PACURRENT before the radio first reports it). (#3646)
    obj["age_ms"] = hasValue ? QJsonValue(ageMs) : QJsonValue(-1);
    return obj;
}

} // namespace

MeterModel::MeterModel(QObject* parent)
    : QObject(parent)
{}

void MeterModel::setTgxlHandle(quint32 handle)
{
    if (m_tgxlHandle == handle) return;
    m_tgxlHandle = handle;

    // Re-scan existing AMP meter definitions to reassign TGXL vs PGXL.
    // Meter definitions may arrive before the TGXL handle is known,
    // causing all AMP meters to be routed to the PGXL slot (#600).
    m_tgxlFwdIdx = -1;
    m_tgxlSwrIdx = -1;
    m_tgxlFwdPwr = 0.0f;
    m_tgxlSwr = 1.0f;
    m_lastTgxlFwdPowerUpdateMs = 0;
    m_lastTgxlSwrUpdateMs = 0;
    m_ampFwdPwrIdx = -1;
    m_ampSwrIdx = -1;
    m_ampTempIdx = -1;
    for (auto it = m_defs.constBegin(); it != m_defs.constEnd(); ++it) {
        const auto& def = *it;
        if (def.source == "AMP" && def.name == "FWD" && def.unit == "dBm") {
            if (handle != 0 && def.sourceIndex == static_cast<int>(handle))
                m_tgxlFwdIdx = def.index;
            else
                m_ampFwdPwrIdx = def.index;
        } else if (def.source == "AMP" && def.name == "RL") {
            if (handle != 0 && def.sourceIndex == static_cast<int>(handle))
                m_tgxlSwrIdx = def.index;
            else
                m_ampSwrIdx = def.index;
        } else if (def.source == "AMP" && def.name == "TEMP") {
            m_ampTempIdx = def.index;
        }
    }
}

void MeterModel::defineMeter(const MeterDef& def)
{
    m_defs[def.index] = def;
    if (def.source == "SLC")
        m_manifestSliceContext = def.sourceIndex;

    // Cache indices for high-frequency lookups
    if (def.source == "SLC" && def.name == "LEVEL")
        m_sLevelIdxBySlice[def.sourceIndex] = def.index;
    else if (def.source == "SLC" && def.name == "ESC")
        m_escLevelIdxBySlice[def.sourceIndex] = def.index;
    else if (def.source.startsWith("TX") && def.name == "FWDPWR")
        m_fwdPwrIdx = def.index;
    else if (def.source.startsWith("TX") && def.name == "REFPWR")
        m_refPwrIdx = def.index;
    else if (def.source.startsWith("TX") && def.name == "SWR")
        m_swrIdx = def.index;
    else if (def.name == "MICPEAK")
        m_micPeakIdx = def.index;
    else if (isTxWaveformMeter(def) && def.name == "COMPPEAK") {
        if (hasExplicitTxWaveformSourceIndex(def))
            m_compPeakIdxByTxSource[def.sourceIndex] = def.index;
        else
            m_compPeakIdxBySlice[implicitTxWaveformSliceIndex()] = def.index;
    }
    else if (def.name == "MIC")
        m_micLevelIdx = def.index;
    else if (def.name == "COMP")
        m_compLevelIdx = def.index;
    else if (def.name == "HWALC")
        m_hwAlcIdx = def.index;
    else if (def.name == "ALC")
        m_swAlcIdx = def.index;
    else if (def.source != "AMP" && def.name == "PATEMP")
        m_paTempIdx = def.index;
    else if (def.name == "+13.8A")
        m_supplyIdx = def.index;
    // Amplifier meters (source "AMP")
    // Multiple FWD/RL meters exist — one per amplifier handle.
    // TGXL meters go to TunerApplet (m_tgxlFwd/SwrIdx).
    // PGXL meters go to AmpApplet (m_ampFwdPwrIdx/SwrIdx/TempIdx).
    // Distinguish by matching def.sourceIndex against the known TGXL handle.
    else if (def.source == "AMP" && def.name == "FWD" && def.unit == "dBm") {
        if (m_tgxlHandle != 0 && def.sourceIndex == static_cast<int>(m_tgxlHandle))
            m_tgxlFwdIdx = def.index;
        else
            m_ampFwdPwrIdx = def.index;
    }
    else if (def.source == "AMP" && def.name == "RL") {
        if (m_tgxlHandle != 0 && def.sourceIndex == static_cast<int>(m_tgxlHandle))
            m_tgxlSwrIdx = def.index;
        else
            m_ampSwrIdx = def.index;
    }
    else if (def.source == "AMP" && def.name == "TEMP")
        m_ampTempIdx = def.index;

    recomputeSourceIndexMins();

    qCDebug(lcMeters) << "MeterModel: defined meter" << def.index
             << def.source << def.sourceIndex << def.name
             << def.unit << "[" << def.low << "->" << def.high << "]";
    if (isTxWaveformMeter(def) && def.name == "COMPPEAK") {
        logCompressionMeterMap(def);
    }
}

void MeterModel::removeMeter(int index)
{
    m_defs.remove(index);
    m_values.remove(index);
    m_valueUpdatedMs.remove(index);

    // Remove from per-slice LEVEL map
    for (auto it = m_sLevelIdxBySlice.begin(); it != m_sLevelIdxBySlice.end(); ) {
        if (it.value() == index) it = m_sLevelIdxBySlice.erase(it);
        else ++it;
    }
    for (auto it = m_escLevelIdxBySlice.begin(); it != m_escLevelIdxBySlice.end(); ) {
        if (it.value() == index) it = m_escLevelIdxBySlice.erase(it);
        else ++it;
    }
    bool compressionMapChanged = false;
    for (auto it = m_compPeakIdxByTxSource.begin(); it != m_compPeakIdxByTxSource.end(); ) {
        if (it.value() == index) {
            it = m_compPeakIdxByTxSource.erase(it);
            compressionMapChanged = true;
        } else {
            ++it;
        }
    }
    for (auto it = m_compPeakIdxBySlice.begin(); it != m_compPeakIdxBySlice.end(); ) {
        if (it.value() == index) {
            it = m_compPeakIdxBySlice.erase(it);
            compressionMapChanged = true;
        } else {
            ++it;
        }
    }
    if (index == m_fwdPwrIdx)   m_fwdPwrIdx = -1;
    if (index == m_refPwrIdx) {
        m_refPwrIdx = -1;
        m_reflectedPower = 0.0f;
        m_lastReflectedPowerUpdateMs = 0;
    }
    if (index == m_swrIdx)      m_swrIdx = -1;
    if (index == m_micPeakIdx)   m_micPeakIdx = -1;
    if (index == m_micLevelIdx)  m_micLevelIdx = -1;
    if (index == m_compLevelIdx) m_compLevelIdx = -1;
    if (index == m_hwAlcIdx)     m_hwAlcIdx = -1;
    if (index == m_swAlcIdx)     m_swAlcIdx = -1;
    if (index == m_paTempIdx)    m_paTempIdx = -1;
    if (index == m_supplyIdx) {
        m_supplyIdx = -1;
        m_hasSupplyVoltsValue = false;   // the sample cannot outlive its meter
    }
    if (index == m_ampFwdPwrIdx) m_ampFwdPwrIdx = -1;
    if (index == m_ampSwrIdx)    m_ampSwrIdx = -1;
    if (index == m_ampTempIdx)   m_ampTempIdx = -1;
    if (index == m_tgxlFwdIdx) {
        m_tgxlFwdIdx = -1;
        m_tgxlFwdPwr = 0.0f;
        m_lastTgxlFwdPowerUpdateMs = 0;
    }
    if (index == m_tgxlSwrIdx) {
        m_tgxlSwrIdx = -1;
        m_tgxlSwr = 1.0f;
        m_lastTgxlSwrUpdateMs = 0;
    }

    recomputeSourceIndexMins();
    if (compressionMapChanged) {
        clearCompressionState();
        logCompressionSummary("meter-removed", true);
    }
}

float MeterModel::convertRaw(const MeterDef& def, qint16 raw) const
{
    // Conversion factors from FlexLib Meter.cs UpdateValue()
    // FlexLib uses volt_denom=1024 for fw < 1.11.0.0, 256 for newer.
    // FLEX-8600 fw v1.4.0.0 is a newer product and uses 256.
    if (def.unit == "dBm" || def.unit == "dB" || def.unit == "dBFS" || def.unit == "SWR")
        return static_cast<float>(raw) / 128.0f;
    if (def.unit == "Volts" || def.unit == "Amps")
        return static_cast<float>(raw) / 256.0f;
    if (def.unit == "degF" || def.unit == "degC")
        return static_cast<float>(raw) / 64.0f;
    return static_cast<float>(raw);
}

bool MeterModel::updateValueByName(const QString& source, const QString& name,
                                   float converted, int sourceIndex)
{
    const int idx = findMeter(source, name, sourceIndex);
    if (idx < 0)
        return false;
    const MeterDef* def = meterDef(idx);
    if (!def)
        return false;

    // Inverse of convertRaw(). Kept adjacent to it so the two cannot drift.
    float scale = 1.0f;
    if (def->unit == "dBm" || def->unit == "dB" || def->unit == "dBFS" || def->unit == "SWR")
        scale = 128.0f;
    else if (def->unit == "Volts" || def->unit == "Amps")
        scale = 256.0f;
    else if (def->unit == "degF" || def->unit == "degC")
        scale = 64.0f;

    const float scaled = converted * scale;
    // Saturate rather than wrap: a wrapped qint16 turns a large positive
    // reading into a large NEGATIVE one, which on a power meter reads as
    // "no output" at exactly the moment there is the most of it.
    const qint16 raw = static_cast<qint16>(
        std::clamp(scaled, -32768.0f, 32767.0f));

    updateValues(QVector<quint16>{static_cast<quint16>(idx)}, QVector<qint16>{raw});
    return true;
}

void MeterModel::clear()
{
    m_defs.clear();
    m_values.clear();
    m_valueUpdatedMs.clear();
    m_sLevelIdxBySlice.clear();
    m_escLevelIdxBySlice.clear();
    m_compPeakIdxByTxSource.clear();
    m_compPeakIdxBySlice.clear();
    m_minSliceSourceIndex = -1;
    m_minTxWaveformSourceIndex = -1;
    m_manifestSliceContext = -1;
    m_activeTxSlice = -1;
    m_fwdPwrIdx = -1;
    m_refPwrIdx = -1;
    m_swrIdx = -1;
    m_micPeakIdx = -1;
    m_micLevelIdx = -1;
    m_compLevelIdx = -1;
    m_hwAlcIdx = -1;
    m_swAlcIdx = -1;
    m_paTempIdx = -1;
    m_supplyIdx = -1;
    m_hasSupplyVoltsValue = false;
    m_ampFwdPwrIdx = -1;
    m_ampSwrIdx = -1;
    m_ampTempIdx = -1;
    m_tgxlFwdIdx = -1;
    m_tgxlSwrIdx = -1;
    m_tgxlHandle = 0;
    m_tgxlFwdPwr = 0.0f;
    m_tgxlSwr = 1.0f;
    m_lastTgxlFwdPowerUpdateMs = 0;
    m_lastTgxlSwrUpdateMs = 0;
    m_sLevel = -130.0f;
    m_fwdPower = 0.0f;
    m_fwdPowerInstant = 0.0f;
    m_reflectedPower = 0.0f;
    m_swr = 1.0f;
    m_lastTxMeterUpdateMs = 0;
    m_lastFwdPowerUpdateMs = 0;
    m_lastReflectedPowerUpdateMs = 0;
    m_lastSwrUpdateMs = 0;
    m_micPeak = -50.0f;
    clearCompressionState();
    m_micLevel = -50.0f;
    m_compLevel = 0.0f;
    m_hwAlc = 0.0f;
    m_swAlc = 0.0f;
    m_paTemp = 0.0f;
    m_supplyVolts = 0.0f;
    m_ampFwdPwr = 0.0f;
    m_ampSwr = 1.0f;
    m_ampTemp = 0.0f;
}

void MeterModel::setActiveTxSlice(int sliceIndex)
{
    if (m_activeTxSlice == sliceIndex)
        return;

    m_activeTxSlice = sliceIndex;
    clearCompressionState();
    logCompressionSummary("active-slice-change", true);
    emit micMetersChanged(m_micLevel, m_compLevel, m_micPeak, m_compPeak);
}

void MeterModel::clearCompressionState()
{
    m_compPeak = 0.0f;
    m_hasCompPeakValue = false;
    m_compPeakLevel = 0.0f;
    m_hasCompPeakLevel = false;
    m_compPeakUpdatedMs = 0;
    m_lastCompressionSummaryLogMs = 0;
    m_lastCompressionSummaryReason.clear();
}

bool MeterModel::isTxWaveformMeter(const MeterDef& def) const
{
    return def.source.startsWith("TX");
}

bool MeterModel::hasExplicitTxWaveformSourceIndex(const MeterDef& def) const
{
    return isTxWaveformMeter(def) && def.sourceIndex >= kMinTxWaveformSourceIndex;
}

int MeterModel::implicitTxWaveformSliceIndex() const
{
    if (m_manifestSliceContext >= 0)
        return m_manifestSliceContext;
    return 0;
}

void MeterModel::recomputeSourceIndexMins()
{
    m_minSliceSourceIndex = -1;
    m_minTxWaveformSourceIndex = -1;

    for (auto it = m_defs.constBegin(); it != m_defs.constEnd(); ++it) {
        const MeterDef& def = *it;
        if (def.source == "SLC") {
            if (m_minSliceSourceIndex < 0 || def.sourceIndex < m_minSliceSourceIndex)
                m_minSliceSourceIndex = def.sourceIndex;
        } else if (hasExplicitTxWaveformSourceIndex(def)) {
            if (m_minTxWaveformSourceIndex < 0 || def.sourceIndex < m_minTxWaveformSourceIndex)
                m_minTxWaveformSourceIndex = def.sourceIndex;
        }
    }
}

int MeterModel::txWaveformBase() const
{
    if (m_minTxWaveformSourceIndex < 0)
        return -1;
    if (m_minSliceSourceIndex >= 0)
        return m_minTxWaveformSourceIndex - m_minSliceSourceIndex;
    return m_minTxWaveformSourceIndex;
}

int MeterModel::activeTxWaveformSourceIndex() const
{
    if (m_activeTxSlice < 0)
        return -1;

    const int base = txWaveformBase();
    if (base < 0)
        return -1;

    return base + m_activeTxSlice;
}

int MeterModel::compPeakIndexForActiveTxSlice() const
{
    const int txSource = activeTxWaveformSourceIndex();
    if (txSource >= 0 && m_compPeakIdxByTxSource.contains(txSource))
        return m_compPeakIdxByTxSource.value(txSource);
    return m_compPeakIdxBySlice.value(m_activeTxSlice, -1);
}

void MeterModel::logCompressionMeterMap(const MeterDef& def) const
{
    if (!lcMeters().isDebugEnabled())
        return;

    const int base = txWaveformBase();
    const int slice = hasExplicitTxWaveformSourceIndex(def)
        ? (base >= 0 ? def.sourceIndex - base : -1)
        : implicitTxWaveformSliceIndex();
    qCDebug(lcMeters) << "MeterModel: compression meter map"
                      << "name" << def.name
                      << "id" << def.index
                      << "txSource" << def.sourceIndex
                      << "txBase" << base
                      << "slice" << slice
                      << "explicitTxSource" << hasExplicitTxWaveformSourceIndex(def)
                      << "description" << def.description;
}

void MeterModel::logCompressionSummary(const char* reason, bool force)
{
    if (!lcMeters().isDebugEnabled())
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString reasonText = QString::fromLatin1(reason ? reason : "");
    const bool reasonChanged = reasonText != m_lastCompressionSummaryReason;
    if (!force && !reasonChanged && now - m_lastCompressionSummaryLogMs < kCompressionSummaryLogIntervalMs)
        return;

    m_lastCompressionSummaryLogMs = now;
    m_lastCompressionSummaryReason = reasonText;

    qCDebug(lcMeters) << "MeterModel: compression summary"
                      << "reason" << reasonText
                      << "activeSlice" << m_activeTxSlice
                      << "txBase" << txWaveformBase()
                      << "txSource" << activeTxWaveformSourceIndex()
                      << "usingImplicitSliceMap"
                      << (!m_compPeakIdxByTxSource.contains(activeTxWaveformSourceIndex())
                          && m_compPeakIdxBySlice.contains(m_activeTxSlice))
                      << "compPeakId" << compPeakIndexForActiveTxSlice()
                      << "compPeakDb" << m_compPeakLevel
                      << "hasCompPeak" << m_hasCompPeakLevel
                      << "displayDb" << m_compPeak
                      << "available" << m_hasCompPeakValue;
}

bool MeterModel::swrSampleLive(qint64 nowMs, qint64 swrMaxAgeMs) const
{
    // See the declaration for the contract. Order matters: the SWR sample's
    // own age is always required — a stale ratio is not a measurement no
    // matter what power is doing (#4533, the 16-minute-old reading).
    const bool swrFresh = m_lastSwrUpdateMs > 0
        && (nowMs - m_lastSwrUpdateMs) <= swrMaxAgeMs;
    if (!swrFresh)
        return false;
    // Backend never published forward power (HL2): SWR self-gates upstream.
    if (m_lastFwdPowerUpdateMs == 0)
        return true;
    // Backend does publish power: a fresh ratio with no qualifying power
    // behind it is two noise samples divided (#4533's saturated 255.99).
    return (nowMs - m_lastFwdPowerUpdateMs) <= kDirectionalMeterFreshnessMs
        && m_fwdPowerInstant > kMinForwardWattsForSwr;
}

std::optional<float> MeterModel::swrIfLive() const
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    return swrSampleLive(now, kTxMeterStaleMs) ? std::optional<float>(m_swr)
                                               : std::nullopt;
}

bool MeterModel::hasRecentTxMeters(qint64 maxAgeMs) const
{
    if (m_lastTxMeterUpdateMs <= 0 || maxAgeMs < 0)
        return false;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    return now - m_lastTxMeterUpdateMs <= maxAgeMs;
}

bool MeterModel::hasRecentReflectedPower(qint64 maxAgeMs) const
{
    if (m_refPwrIdx < 0 || m_lastReflectedPowerUpdateMs <= 0 || maxAgeMs < 0) {
        return false;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    return now - m_lastReflectedPowerUpdateMs <= maxAgeMs;
}

void MeterModel::updateValues(const QVector<quint16>& ids, const QVector<qint16>& vals)
{
    const int n = qMin(ids.size(), vals.size());
    const qint64 packetUpdatedMs = QDateTime::currentMSecsSinceEpoch();
    const int activeCompPeakIdx = compPeakIndexForActiveTxSlice();
    // sLevelChanged is emitted per-slice inline in the loop below
    bool txChanged = false;
    bool directionalChanged = false;
    bool fwdInstantChanged = false;
    bool micChanged = false;
    bool hwAlcChangedFlag = false;
    bool swAlcChangedFlag = false;
    bool hwChanged = false;
    bool ampChanged = false;
    bool tgxlChanged = false;

    for (int i = 0; i < n; ++i) {
        const int idx = static_cast<int>(ids[i]);
        auto it = m_defs.constFind(idx);
        if (it == m_defs.constEnd()) continue;

        const float v = convertRaw(*it, vals[i]);
        m_values[idx] = v;
        m_valueUpdatedMs[idx] = packetUpdatedMs;  // per-meter freshness (#3646)

        // Check if this meter is a per-slice LEVEL meter
        bool isSliceLevel = false;
        for (auto sit = m_sLevelIdxBySlice.constBegin(); sit != m_sLevelIdxBySlice.constEnd(); ++sit) {
            if (sit.value() == idx) {
                emit sLevelChanged(sit.key(), v);
                isSliceLevel = true;
                break;
            }
        }
        // Check if this meter is a per-slice ESC meter
        if (!isSliceLevel) {
            for (auto sit = m_escLevelIdxBySlice.constBegin(); sit != m_escLevelIdxBySlice.constEnd(); ++sit) {
                if (sit.value() == idx) {
                    emit escLevelChanged(sit.key(), v);
                    isSliceLevel = true;
                    break;
                }
            }
        }
        if (isSliceLevel) {
            // no-op, already emitted
        } else if (idx == m_fwdPwrIdx) {
            m_lastTxMeterUpdateMs = packetUpdatedMs;
            m_lastFwdPowerUpdateMs = m_lastTxMeterUpdateMs;
            // FWDPWR meter reports in dBm — convert to watts for display.
            // watts = 10^(dBm/10) / 1000
            // e.g. 50 dBm = 100 W, 47 dBm ≈ 50 W, 40 dBm = 10 W
            float watts = std::pow(10.0f, v / 10.0f) / 1000.0f;
            m_fwdPowerInstant = watts;
            fwdInstantChanged = true;
            directionalChanged = true;
            // Smooth: fast attack (α=0.5) to track peaks, slow decay (α=0.15)
            // for stable display without jitter (#980)
            //
            // The slow decay is right DURING a transmission and wrong at the end
            // of one. On unkey the radio reports 0 dBm, which is 0.001 W rather
            // than 0, so the filter creeps towards it at 15 % per sample instead
            // of arriving: measured on a FLEX-6700, the dBm meter read 0 within
            // 200 ms while the watts reading was still 3.45 W, and it took
            // ~2.9 s to fall away. For that whole window the display claims
            // forward power out of a radio that has stopped transmitting.
            //
            // So: keep the smoothing for real readings, but snap to zero once
            // the meter says there is no carrier. REFPWR immediately below is
            // not smoothed at all and drops instantly — this brings the two
            // directional readings back into agreement instead of having one
            // linger while the other is already at rest.
            if (watts <= kNoCarrierWatts) {
                m_fwdPower = 0.0f;
            } else if (m_fwdPower < 0.01f) {
                m_fwdPower = watts;  // first sample — no smoothing
            } else {
                float alpha = (watts > m_fwdPower) ? 0.5f : 0.15f;
                m_fwdPower = alpha * watts + (1.0f - alpha) * m_fwdPower;
            }
            txChanged = true;
        } else if (idx == m_refPwrIdx) {
            m_lastTxMeterUpdateMs = packetUpdatedMs;
            m_lastReflectedPowerUpdateMs = m_lastTxMeterUpdateMs;
            // REFPWR is an independent directional-coupler reading in dBm.
            // Preserve it as watts rather than reconstructing it from SWR.
            m_reflectedPower = std::pow(10.0f, v / 10.0f) / 1000.0f;
            directionalChanged = true;
        } else if (idx == m_swrIdx) {
            m_lastTxMeterUpdateMs = packetUpdatedMs;
            m_lastSwrUpdateMs = m_lastTxMeterUpdateMs;
            m_swr = v;
            txChanged = true;
            directionalChanged = true;
        } else if (idx == m_micPeakIdx) {
            m_micPeak = v;
            micChanged = true;
        } else if (idx == activeCompPeakIdx) {
            m_compPeakLevel = v;
            m_hasCompPeakLevel = true;
            m_compPeakUpdatedMs = packetUpdatedMs;
            m_compPeak = compressionValueForGauge(v);
            m_hasCompPeakValue = true;
            logCompressionSummary("ok");
            micChanged = true;
        } else if (idx == m_micLevelIdx) {
            m_micLevel = v;
            micChanged = true;
        } else if (idx == m_compLevelIdx) {
            m_compLevel = v;
            micChanged = true;
        } else if (idx == m_hwAlcIdx) {
            m_hwAlc = v;
            hwAlcChangedFlag = true;
        } else if (idx == m_swAlcIdx) {
            m_swAlc = v;
            swAlcChangedFlag = true;
        } else if (idx == m_paTempIdx) {
            m_paTemp = v;
            hwChanged = true;
        } else if (idx == m_supplyIdx) {
            m_supplyVolts = v;  // "+13.8A" = supply voltage at point A (before fuse)
            m_hasSupplyVoltsValue = true;   // a SAMPLE, not just a definition
            hwChanged = true;
        } else if (idx == m_tgxlFwdIdx) {
            m_tgxlFwdPwr = std::pow(10.0f, v / 10.0f) / 1000.0f;
            m_lastTgxlFwdPowerUpdateMs = packetUpdatedMs;
            tgxlChanged = true;
        } else if (idx == m_tgxlSwrIdx) {
            float rho = std::pow(10.0f, -v / 20.0f);
            m_tgxlSwr = (rho < 0.999f) ? (1.0f + rho) / (1.0f - rho) : 99.9f;
            m_lastTgxlSwrUpdateMs = packetUpdatedMs;
            tgxlChanged = true;
        } else if (idx == m_ampFwdPwrIdx) {
            m_ampFwdPwr = std::pow(10.0f, v / 10.0f) / 1000.0f;  // dBm → watts
            ampChanged = true;
        } else if (idx == m_ampSwrIdx) {
            float rho = std::pow(10.0f, -v / 20.0f);
            m_ampSwr = (rho < 0.999f) ? (1.0f + rho) / (1.0f - rho) : 99.9f;
            ampChanged = true;
        } else if (idx == m_ampTempIdx) {
            m_ampTemp = v;
            ampChanged = true;
        }

        emit meterUpdated(idx, v);
    }

    // SWR as published to consumers. It is derived from forward and reflected
    // power, so once the TX meters go stale it is not a measurement any more —
    // it is whatever was last read during a previous transmit, and on a radio
    // that publishes SWR but no FWDPWR it saturates at 255.99 and stays there
    // (#4533).
    //
    // Gated HERE, at the single point both signals read it, rather than in each
    // consumer: HealthApplet already qualifies SWR on instantaneous forward
    // power (#4243), but that gate cannot fire on a backend which never sends
    // FWDPWR at all, so every such consumer is defeated by the absence of the
    // very quantity it gates on.
    //
    // ⚠ Gate on SWR'S OWN AGE (swrUpdatedAtMs), NOT on forward power and NOT
    // on hasRecentTxMeters(). The earlier forward-power gate silently zeroed
    // SWR forever on any backend that publishes SWR without FWDPWR — the HL2
    // does exactly that, by design (its forward counts are uncalibrated ADC
    // values; the RATIO survives the unknown scale, which is why its SWR is
    // the most trustworthy meter it has). "This SWR reading is old" is the
    // claim this gate makes; power flowing is evidence for a different
    // proposition, and #4243's HealthApplet already qualifies on power where
    // power data exists.
    //
    // ⚠ The two emits stay CO-EMITTED and in this order — #4243 depends on
    // directionalPowerMetersChanged landing in the same cycle as
    // txMetersChanged. Only the values carried change here, never the timing.
    const bool swrValid =
        m_swrIdx >= 0
        && swrSampleLive(packetUpdatedMs, kDirectionalMeterFreshnessMs);
    // 0.0f is a PLACEHOLDER carried alongside swrValid=false, never a value:
    // RadioSwrValidityFilter reads <1.0 as the radio's over-range sentinel and
    // other consumers clamp it to 1.0, so an unaccompanied 0.0f means opposite
    // things on different surfaces (#4536 review, blocker 2).
    const float publishedSwr = swrValid ? m_swr : 0.0f;

    // sLevelChanged is now emitted per-slice inline above
    if (txChanged)
        emit txMetersChanged(m_fwdPower, publishedSwr, swrValid);
    if (directionalChanged) {
        const bool reflectedPowerMeasured = m_refPwrIdx >= 0
            && m_lastReflectedPowerUpdateMs > 0
            && packetUpdatedMs - m_lastReflectedPowerUpdateMs
                <= kDirectionalMeterFreshnessMs;
        emit directionalPowerMetersChanged(m_fwdPowerInstant,
                                           m_reflectedPower,
                                           publishedSwr,
                                           swrValid,
                                           reflectedPowerMeasured);
    }
    // Separate signal carries the raw pre-smoothed sample so consumers
    // can compute PEP peak-hold without re-tracking the smoothing
    // ballistics. (#2561)
    if (fwdInstantChanged)
        emit txPeakChanged(m_fwdPowerInstant);
    if (micChanged)
        emit micMetersChanged(m_micLevel, m_compLevel, m_micPeak, m_compPeak);
    if (hwAlcChangedFlag)
        emit this->hwAlcChanged(m_hwAlc);
    if (swAlcChangedFlag)
        emit this->swAlcChanged(m_swAlc);
    if (hwChanged)
        emit hwTelemetryChanged(m_paTemp, m_supplyVolts);
    if (ampChanged)
        emit ampMetersChanged(m_ampFwdPwr, m_ampSwr, m_ampTemp);
    if (tgxlChanged)
        emit tgxlMetersChanged(m_tgxlFwdPwr, m_tgxlSwr);
}

const MeterDef* MeterModel::meterDef(int index) const
{
    auto it = m_defs.constFind(index);
    return (it != m_defs.constEnd()) ? &(*it) : nullptr;
}

int MeterModel::findMeter(const QString& source, const QString& name, int sourceIndex) const
{
    for (auto it = m_defs.constBegin(); it != m_defs.constEnd(); ++it) {
        if (it->source == source && it->name == name) {
            if (sourceIndex < 0 || it->sourceIndex == sourceIndex)
                return it->index;
        }
    }
    return -1;
}

float MeterModel::value(int index) const
{
    return m_values.value(index, 0.0f);
}

QJsonArray MeterModel::allMeters() const
{
    QJsonArray meters;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // One predicate for every surface — see swrSampleLive() (#4536).
    const bool swrFresh = swrSampleLive(now, kTxMeterStaleMs);
    for (auto it = m_defs.constBegin(); it != m_defs.constEnd(); ++it) {
        const auto valueIt = m_values.constFind(it.key());
        bool hasValue = valueIt != m_values.constEnd();
        // SWR is derived from forward and reflected power, so once the TX
        // meters go stale it is not a measurement any more — it is the last
        // reading from a previous transmit. Reporting it as a live value sends
        // the operator hunting for an antenna fault that isn't there (#4533).
        // Present it the same way FWDPWR/REFPWR already present themselves when
        // the radio isn't sending them: has_value=false, value=null, age=-1.
        // Matches the existing gates in RigctlProtocol (which additionally
        // requires an active transmit) and the Amp/Acom applets, both of which
        // blank SWR without drive.
        if (hasValue && it.key() == m_swrIdx && !swrFresh)
            hasValue = false;
        const qint64 upd = m_valueUpdatedMs.value(it.key(), 0);
        const qint64 ageMs = (hasValue && upd > 0) ? (now - upd) : -1;
        meters.append(meterToJson(*it, hasValue, hasValue ? valueIt.value() : 0.0f, ageMs));
    }
    return meters;
}

QJsonArray MeterModel::metersForSource(const QString& source, int sourceIndex) const
{
    QJsonArray meters;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // One predicate for every surface — see swrSampleLive() (#4536).
    const bool swrFresh = swrSampleLive(now, kTxMeterStaleMs);
    for (auto it = m_defs.constBegin(); it != m_defs.constEnd(); ++it) {
        const MeterDef& def = *it;
        if (def.source != source)
            continue;
        if (sourceIndex >= 0 && def.sourceIndex != sourceIndex)
            continue;

        const auto valueIt = m_values.constFind(it.key());
        bool hasValue = valueIt != m_values.constEnd();
        // Same SWR gate as allMeters(). Without it a `tx`-source query answered
        // has_value=true for the identical meter that `all` reports as absent —
        // two views of one model disagreeing about the same reading, which is
        // the disagreement #4533 was filed about rather than a second bug.
        if (hasValue && it.key() == m_swrIdx && !swrFresh)
            hasValue = false;
        const qint64 upd = m_valueUpdatedMs.value(it.key(), 0);
        const qint64 ageMs = (hasValue && upd > 0) ? (now - upd) : -1;
        meters.append(meterToJson(def, hasValue, hasValue ? valueIt.value() : 0.0f, ageMs));
    }
    return meters;
}

} // namespace AetherSDR
