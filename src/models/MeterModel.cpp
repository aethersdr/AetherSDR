#include "MeterModel.h"
#include "core/LogManager.h"
#include <QDebug>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr qint64 kCompressionReferenceMaxSkewMs = 250;
constexpr qint64 kCompressionSummaryLogIntervalMs = 500;
constexpr int kMinTxWaveformSourceIndex = 8;

float compressionReductionForGauge(float referenceDbfs, float compPeakDbfs)
{
    // COMPPEAK is a dBFS level tap at the speech processor/clipper stage.
    // SmartSDR-style compression tracks the lift from the model-specific
    // processor reference tap to COMPPEAK.
    // The gauge is reversed and uses negative values internally:
    //   0 = none, -25 = heavy.
    const float reduction = qMax(0.0f, compPeakDbfs - referenceDbfs);
    return -qBound(0.0f, reduction, 25.0f);
}

QJsonObject meterToJson(const MeterDef& def, bool hasValue, float value)
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
    else if (isTxWaveformMeter(def) && def.name == "AFTEREQ") {
        if (hasExplicitTxWaveformSourceIndex(def))
            m_afterEqIdxByTxSource[def.sourceIndex] = def.index;
        else
            m_afterEqIdxBySlice[implicitTxWaveformSliceIndex()] = def.index;
    }
    else if (isTxWaveformMeter(def) && def.name == "SC_MIC") {
        if (hasExplicitTxWaveformSourceIndex(def))
            m_scMicIdxByTxSource[def.sourceIndex] = def.index;
        else
            m_scMicIdxBySlice[implicitTxWaveformSliceIndex()] = def.index;
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
    if (isTxWaveformMeter(def)
        && (def.name == "COMPPEAK" || def.name == "AFTEREQ" || def.name == "SC_MIC")) {
        logCompressionMeterMap(def);
    }
}

void MeterModel::removeMeter(int index)
{
    m_defs.remove(index);
    m_values.remove(index);

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
    for (auto it = m_afterEqIdxByTxSource.begin(); it != m_afterEqIdxByTxSource.end(); ) {
        if (it.value() == index) {
            it = m_afterEqIdxByTxSource.erase(it);
            compressionMapChanged = true;
        } else {
            ++it;
        }
    }
    for (auto it = m_afterEqIdxBySlice.begin(); it != m_afterEqIdxBySlice.end(); ) {
        if (it.value() == index) {
            it = m_afterEqIdxBySlice.erase(it);
            compressionMapChanged = true;
        } else {
            ++it;
        }
    }
    for (auto it = m_scMicIdxByTxSource.begin(); it != m_scMicIdxByTxSource.end(); ) {
        if (it.value() == index) {
            it = m_scMicIdxByTxSource.erase(it);
            compressionMapChanged = true;
        } else {
            ++it;
        }
    }
    for (auto it = m_scMicIdxBySlice.begin(); it != m_scMicIdxBySlice.end(); ) {
        if (it.value() == index) {
            it = m_scMicIdxBySlice.erase(it);
            compressionMapChanged = true;
        } else {
            ++it;
        }
    }
    if (index == m_fwdPwrIdx)   m_fwdPwrIdx = -1;
    if (index == m_swrIdx)      m_swrIdx = -1;
    if (index == m_micPeakIdx)   m_micPeakIdx = -1;
    if (index == m_micLevelIdx)  m_micLevelIdx = -1;
    if (index == m_compLevelIdx) m_compLevelIdx = -1;
    if (index == m_hwAlcIdx)     m_hwAlcIdx = -1;
    if (index == m_swAlcIdx)     m_swAlcIdx = -1;
    if (index == m_paTempIdx)    m_paTempIdx = -1;
    if (index == m_supplyIdx)    m_supplyIdx = -1;
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
        updateCompressionReduction();
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

void MeterModel::clear()
{
    m_defs.clear();
    m_values.clear();
    m_sLevelIdxBySlice.clear();
    m_escLevelIdxBySlice.clear();
    m_compPeakIdxByTxSource.clear();
    m_afterEqIdxByTxSource.clear();
    m_scMicIdxByTxSource.clear();
    m_compPeakIdxBySlice.clear();
    m_afterEqIdxBySlice.clear();
    m_scMicIdxBySlice.clear();
    m_minSliceSourceIndex = -1;
    m_minTxWaveformSourceIndex = -1;
    m_manifestSliceContext = -1;
    m_activeTxSlice = -1;
    m_fwdPwrIdx = -1;
    m_swrIdx = -1;
    m_micPeakIdx = -1;
    m_micLevelIdx = -1;
    m_compLevelIdx = -1;
    m_hwAlcIdx = -1;
    m_swAlcIdx = -1;
    m_paTempIdx = -1;
    m_supplyIdx = -1;
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
    m_swr = 1.0f;
    m_lastTxMeterUpdateMs = 0;
    m_lastFwdPowerUpdateMs = 0;
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
    m_afterEqLevel = -150.0f;
    m_scMicLevel = -150.0f;
    m_compPeakLevel = -150.0f;
    m_hasAfterEqLevel = false;
    m_hasScMicLevel = false;
    m_hasCompPeakLevel = false;
    m_afterEqUpdatedMs = 0;
    m_scMicUpdatedMs = 0;
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

int MeterModel::afterEqIndexForActiveTxSlice() const
{
    const int txSource = activeTxWaveformSourceIndex();
    if (txSource >= 0 && m_afterEqIdxByTxSource.contains(txSource))
        return m_afterEqIdxByTxSource.value(txSource);
    return m_afterEqIdxBySlice.value(m_activeTxSlice, -1);
}

int MeterModel::scMicIndexForActiveTxSlice() const
{
    const int txSource = activeTxWaveformSourceIndex();
    if (txSource >= 0 && m_scMicIdxByTxSource.contains(txSource))
        return m_scMicIdxByTxSource.value(txSource);
    return m_scMicIdxBySlice.value(m_activeTxSlice, -1);
}

void MeterModel::updateCompressionReduction()
{
    if (!m_hasCompPeakLevel) {
        m_compPeak = 0.0f;
        m_hasCompPeakValue = false;
        logCompressionSummary("missing-comppeak");
        return;
    }

    float referenceLevel = 0.0f;
    qint64 referenceUpdatedMs = 0;

    // Flex meter manifests differ by radio family:
    // - FLEX-8000 series exposes TX/AFTEREQ at 20 fps. Captures against
    //   SmartSDR show this is the post-EQ processor input reference, so the
    //   compression display is the lift from AFTEREQ to TX/COMPPEAK.
    // - FLEX-6600 captures do not expose AFTEREQ. The best matching reference
    //   is TX/SC_MIC at 10 fps, with TX/COMPPEAK still at 20 fps. That mixed
    //   cadence can compare a fresh COMPPEAK against a stale SC_MIC sample, so
    //   compressionSamplesFresh() gates the derived 6000-series value. If an
    //   8000-style AFTEREQ exists, prefer it over SC_MIC.
    const int afterEqIdx = afterEqIndexForActiveTxSlice();
    const int scMicIdx = scMicIndexForActiveTxSlice();

    if (afterEqIdx >= 0) {
        if (!m_hasAfterEqLevel) {
            m_compPeak = 0.0f;
            m_hasCompPeakValue = false;
            logCompressionSummary("missing-aftereq");
            return;
        }
        referenceLevel = m_afterEqLevel;
        referenceUpdatedMs = m_afterEqUpdatedMs;
    } else if (scMicIdx >= 0) {
        if (!m_hasScMicLevel) {
            m_compPeak = 0.0f;
            m_hasCompPeakValue = false;
            logCompressionSummary("missing-scmic");
            return;
        }
        referenceLevel = m_scMicLevel;
        referenceUpdatedMs = m_scMicUpdatedMs;
    } else {
        m_compPeak = 0.0f;
        m_hasCompPeakValue = false;
        logCompressionSummary("missing-reference");
        return;
    }

    if (!compressionSamplesFresh(referenceUpdatedMs)) {
        m_compPeak = 0.0f;
        m_hasCompPeakValue = false;
        logCompressionSummary("stale-reference");
        return;
    }

    m_compPeak = compressionReductionForGauge(referenceLevel, m_compPeakLevel);
    m_hasCompPeakValue = true;
    logCompressionSummary("ok");
}

bool MeterModel::compressionSamplesFresh(qint64 referenceUpdatedMs) const
{
    if (m_compPeakUpdatedMs <= 0 || referenceUpdatedMs <= 0)
        return false;

    const qint64 skewMs = m_compPeakUpdatedMs > referenceUpdatedMs
        ? m_compPeakUpdatedMs - referenceUpdatedMs
        : referenceUpdatedMs - m_compPeakUpdatedMs;
    return skewMs <= kCompressionReferenceMaxSkewMs;
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

    const int afterEqIdx = afterEqIndexForActiveTxSlice();
    const int scMicIdx = scMicIndexForActiveTxSlice();
    const bool useAfterEq = afterEqIdx >= 0;
    const bool useScMic = !useAfterEq && scMicIdx >= 0;
    const QString referenceName = useAfterEq
        ? QStringLiteral("AFTEREQ")
        : (useScMic ? QStringLiteral("SC_MIC") : QStringLiteral("none"));
    const int referenceIdx = useAfterEq ? afterEqIdx : (useScMic ? scMicIdx : -1);
    const float referenceDbfs = useAfterEq ? m_afterEqLevel : (useScMic ? m_scMicLevel : 0.0f);
    const bool hasReference = useAfterEq ? m_hasAfterEqLevel : (useScMic && m_hasScMicLevel);
    const qint64 referenceUpdatedMs = useAfterEq ? m_afterEqUpdatedMs : (useScMic ? m_scMicUpdatedMs : 0);

    qint64 skewMs = -1;
    if (m_compPeakUpdatedMs > 0 && referenceUpdatedMs > 0) {
        skewMs = m_compPeakUpdatedMs > referenceUpdatedMs
            ? m_compPeakUpdatedMs - referenceUpdatedMs
            : referenceUpdatedMs - m_compPeakUpdatedMs;
    }

    const float liftDb = (m_hasCompPeakLevel && hasReference)
        ? qMax(0.0f, m_compPeakLevel - referenceDbfs)
        : 0.0f;

    qCDebug(lcMeters) << "MeterModel: compression summary"
                      << "reason" << reasonText
                      << "activeSlice" << m_activeTxSlice
                      << "txBase" << txWaveformBase()
                      << "txSource" << activeTxWaveformSourceIndex()
                      << "usingImplicitSliceMap"
                      << (!m_compPeakIdxByTxSource.contains(activeTxWaveformSourceIndex())
                          && m_compPeakIdxBySlice.contains(m_activeTxSlice))
                      << "reference" << referenceName
                      << "referenceId" << referenceIdx
                      << "referenceDbfs" << referenceDbfs
                      << "hasReference" << hasReference
                      << "compPeakId" << compPeakIndexForActiveTxSlice()
                      << "compPeakDbfs" << m_compPeakLevel
                      << "hasCompPeak" << m_hasCompPeakLevel
                      << "skewMs" << skewMs
                      << "fresh" << compressionSamplesFresh(referenceUpdatedMs)
                      << "liftDb" << liftDb
                      << "displayDb" << m_compPeak
                      << "available" << m_hasCompPeakValue;
}

bool MeterModel::hasRecentTxMeters(qint64 maxAgeMs) const
{
    if (m_lastTxMeterUpdateMs <= 0 || maxAgeMs < 0)
        return false;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    return now - m_lastTxMeterUpdateMs <= maxAgeMs;
}

void MeterModel::updateValues(const QVector<quint16>& ids, const QVector<qint16>& vals)
{
    const int n = qMin(ids.size(), vals.size());
    const qint64 packetUpdatedMs = QDateTime::currentMSecsSinceEpoch();
    const int activeCompPeakIdx = compPeakIndexForActiveTxSlice();
    const int activeAfterEqIdx = afterEqIndexForActiveTxSlice();
    const int activeScMicIdx = scMicIndexForActiveTxSlice();
    // sLevelChanged is emitted per-slice inline in the loop below
    bool txChanged = false;
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
            // Smooth: fast attack (α=0.5) to track peaks, slow decay (α=0.15)
            // for stable display without jitter (#980)
            if (m_fwdPower < 0.01f) {
                m_fwdPower = watts;  // first sample — no smoothing
            } else {
                float alpha = (watts > m_fwdPower) ? 0.5f : 0.15f;
                m_fwdPower = alpha * watts + (1.0f - alpha) * m_fwdPower;
            }
            txChanged = true;
        } else if (idx == m_swrIdx) {
            m_lastTxMeterUpdateMs = packetUpdatedMs;
            m_lastSwrUpdateMs = m_lastTxMeterUpdateMs;
            m_swr = v;
            txChanged = true;
        } else if (idx == m_micPeakIdx) {
            m_micPeak = v;
            micChanged = true;
        } else if (idx == activeCompPeakIdx) {
            // COMPPEAK is a dBFS level tap. Pair it with AFTEREQ on 8000
            // series radios, or SC_MIC on 6000-series radios that do not
            // expose AFTEREQ.
            m_compPeakLevel = v;
            m_hasCompPeakLevel = true;
            m_compPeakUpdatedMs = packetUpdatedMs;
            updateCompressionReduction();
            micChanged = true;
        } else if (idx == activeAfterEqIdx) {
            m_afterEqLevel = v;
            m_hasAfterEqLevel = true;
            m_afterEqUpdatedMs = packetUpdatedMs;
            updateCompressionReduction();
            micChanged = true;
        } else if (idx == activeScMicIdx) {
            m_scMicLevel = v;
            m_hasScMicLevel = true;
            m_scMicUpdatedMs = packetUpdatedMs;
            updateCompressionReduction();
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

    // sLevelChanged is now emitted per-slice inline above
    if (txChanged)
        emit txMetersChanged(m_fwdPower, m_swr);
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
    for (auto it = m_defs.constBegin(); it != m_defs.constEnd(); ++it) {
        const auto valueIt = m_values.constFind(it.key());
        const bool hasValue = valueIt != m_values.constEnd();
        meters.append(meterToJson(*it, hasValue, hasValue ? valueIt.value() : 0.0f));
    }
    return meters;
}

QJsonArray MeterModel::metersForSource(const QString& source, int sourceIndex) const
{
    QJsonArray meters;
    for (auto it = m_defs.constBegin(); it != m_defs.constEnd(); ++it) {
        const MeterDef& def = *it;
        if (def.source != source)
            continue;
        if (sourceIndex >= 0 && def.sourceIndex != sourceIndex)
            continue;

        const auto valueIt = m_values.constFind(it.key());
        const bool hasValue = valueIt != m_values.constEnd();
        meters.append(meterToJson(def, hasValue, hasValue ? valueIt.value() : 0.0f));
    }
    return meters;
}

} // namespace AetherSDR
