#include "PanadapterModel.h"
#include "core/LogManager.h"
#include "core/PerfTelemetry.h"
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <limits>

namespace AetherSDR {

PanadapterModel::PanadapterModel(const QString& panId, QObject* parent)
    : QObject(parent)
    , m_panId(panId)
{}

quint32 PanadapterModel::panStreamId() const
{
    bool ok = false;
    quint32 id = m_panId.toUInt(&ok, 0);  // base 0: auto-detect 0x prefix
    return ok ? id : 0;
}

quint32 PanadapterModel::wfStreamId() const
{
    bool ok = false;
    quint32 id = m_waterfallId.toUInt(&ok, 0);  // base 0: auto-detect 0x prefix
    return ok ? id : 0;
}

void PanadapterModel::setWaterfallId(const QString& id)
{
    if (m_waterfallId != id) {
        ++m_controlRevision;
        m_waterfallId = id;
        emit waterfallIdChanged(id);
    }
}

namespace {
// Handles arrive either as bare lowercase hex (our own assignment) or as the
// radio's "0x…" status form. Hex-only on purpose — parseStatusHandle() in
// core/StreamStatus.h accepts decimal, which would misread bare-hex "40000000".
quint32 parseHandleHex(const QString& text)
{
    QStringView v(text);
    if (v.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        v = v.mid(2);
    }
    bool ok = false;
    const quint32 parsed = v.toUInt(&ok, 16);
    return ok ? parsed : 0;
}
}  // namespace

void PanadapterModel::setClientHandle(const QString& h)
{
    const QPointer<PanadapterModel> self(this);
    setOwnerHandle(parseHandleHex(h));
    if (!self) { return; }
    // Native backends historically leave the client handle unset.
    m_ownerObserved = m_ownerHandle != 0;
}

void PanadapterModel::setOwnerHandle(quint32 handle)
{
    const bool ownerChanged = !m_ownerObserved || m_ownerHandle != handle;
    m_ownerObserved = true;
    if (ownerChanged) {
        ++m_controlRevision;
        ++m_ownershipRevision;
        m_reportedCenterHz.reset();
        m_reportedBandwidthHz.reset();
    }
    m_ownerHandle = handle;
    m_clientHandle = QString::number(handle, 16);
    if (ownerChanged) { emit geometryObservationChanged(); }
}

bool PanadapterModel::ownedByClient(quint32 handle) const
{
    return m_ownerObserved ? handle != 0 && m_ownerHandle == handle
                           : m_ownerHandle == 0 || m_ownerHandle == handle;
}

void PanadapterModel::setRfGainInfo(int low, int high, int step,
                                    const QString& unitSuffix)
{
    m_rfGainLow = low;
    m_rfGainHigh = high;
    m_rfGainStep = step;
    m_rfGainUnitSuffix = unitSuffix;
    m_rfGainRangePublished = true;
    emit rfGainInfoChanged(low, high, step, unitSuffix);
}

// Change-gated, all four. The Icom backend republishes its front-end
// description on every reconnect and the labels are identical each time; an
// ungated emit would rebuild the buttons for no reason.
void PanadapterModel::setPreampLabels(const QStringList& labels)
{
    if (labels == m_preampLabels)
        return;
    m_preampLabels = labels;
    emit preampLabelsChanged(m_preampLabels);
}

void PanadapterModel::setPreampStep(int step)
{
    if (step == m_preampStep)
        return;
    m_preampStep = step;
    emit preampStepChanged(m_preampStep);
}

void PanadapterModel::setAttenuatorLabels(const QStringList& labels)
{
    if (labels == m_attenuatorLabels)
        return;
    m_attenuatorLabels = labels;
    emit attenuatorLabelsChanged(m_attenuatorLabels);
}

void PanadapterModel::setAttenuatorStep(int step)
{
    if (step == m_attenuatorStep)
        return;
    m_attenuatorStep = step;
    emit attenuatorStepChanged(m_attenuatorStep);
}

void PanadapterModel::setScopeModeLabels(const QStringList& labels)
{
    if (labels == m_scopeModeLabels) {
        return;
    }
    m_scopeModeLabels = labels;
    emit scopeModeLabelsChanged(m_scopeModeLabels);
}

void PanadapterModel::setScopeModeIndex(int index)
{
    if (index == m_scopeModeIndex) {
        return;
    }
    m_scopeModeIndex = index;
    emit scopeModeIndexChanged(m_scopeModeIndex);
}

bool PanadapterModel::setCenterBandwidth(double centerMhz, double bandwidthMhz)
{
    bool changed = false;
    if (centerMhz >= 0.0) {
        // The numeric default is a plausible real 20 m center. Track whether
        // a producer has supplied a center so consumers never mistake the
        // placeholder for radio/model state (#3913 review).
        changed = !m_centerKnown;
        m_centerKnown = true;
        if (centerMhz != m_centerMhz) {
            m_centerMhz = centerMhz;
            changed = true;
        }
    }
    if (bandwidthMhz >= 0.0 && bandwidthMhz != m_bandwidthMhz) {
        m_bandwidthMhz = bandwidthMhz;
        changed = true;
    }
    if (changed) {
        ++m_geometryRevision;
        emit infoChanged(m_centerMhz, m_bandwidthMhz);
    }
    return changed;
}

void PanadapterModel::recordGeometryObservation(double centerMhz, double bandwidthMhz)
{
    const auto beforeCenter = m_reportedCenterHz;
    const auto beforeBandwidth = m_reportedBandwidthHz;
    const auto record = [](double mhz, std::optional<qint64>& field) {
        if (mhz < 0 && std::isfinite(mhz)) {
            return; // normalized absent-field sentinel
        }
        // Unlike the legacy display setter, non-finite reports invalidate
        // control observations: retaining old geometry would admit stale intents.
        field = std::isfinite(mhz) && mhz >= 0.000001 && mhz <= 9'007'199'254.0
            ? std::optional<qint64>(qRound64(mhz * 1'000'000.0)) : std::nullopt;
    };
    record(centerMhz, m_reportedCenterHz);
    record(bandwidthMhz, m_reportedBandwidthHz);
    if (beforeCenter != m_reportedCenterHz || beforeBandwidth != m_reportedBandwidthHz) {
        emit geometryObservationChanged();
    }
}

void PanadapterModel::resetCenterKnownForReconnect()
{
    m_lifecyclePublished = false;
    ++m_controlRevision;
    m_displayIntentRevisions.fill(0);
    ++m_ownershipRevision;
    ++m_geometryRevision;
    m_centerKnown = false;
    if (m_reportedCenterHz || m_reportedBandwidthHz) {
        m_reportedCenterHz.reset();
        m_reportedBandwidthHz.reset();
        emit geometryObservationChanged();
    }
}

// Re-announce the current centre/span even though neither changed.
//
// The setter above is change-gated, which is right for status echoes but wrong
// for a backend REFUSING a request: the view applies a zoom optimistically, so
// when the hardware snaps that request back to the span it already had, an
// unchanged model value is precisely the case where the widget is left showing a
// width the data never had. The backend has to be able to say "no, it is still
// this" and have the view follow. (#4470)
void PanadapterModel::republishCenterBandwidth()
{
    if (!m_centerKnown) {
        return;
    }
    emit infoChanged(m_centerMhz, m_bandwidthMhz);
}

bool PanadapterModel::setRange(double minDbm, double maxDbm)
{
    bool changed = false;
    // NaN means "leave unchanged" — the radio may report one bound without the
    // other, and dBm is signed so a numeric sentinel would be ambiguous.
    if (!std::isnan(minDbm) && float(minDbm) != m_minDbm) {
        m_minDbm = float(minDbm);
        changed = true;
    }
    if (!std::isnan(maxDbm) && float(maxDbm) != m_maxDbm) {
        m_maxDbm = float(maxDbm);
        changed = true;
    }
    if (changed) {
        emit levelChanged(m_minDbm, m_maxDbm);
    }
    return changed;
}

bool PanadapterModel::setBandwidthLimits(double minMhz, double maxMhz)
{
    // Both bounds must be positive and ordered to be a limit at all. A partial
    // or inverted report is REJECTED rather than half-applied: the consumer
    // clamps the operator's zoom against this pair, and a min above its max
    // would pin the span to one value with no way out. Unlike setRange's
    // per-bound NaN sentinel, span limits only make sense together.
    if (!(minMhz > 0.0) || !(maxMhz > 0.0) || minMhz > maxMhz) {
        return false;
    }
    if (qFuzzyCompare(minMhz, m_minBandwidthMhz)
        && qFuzzyCompare(maxMhz, m_maxBandwidthMhz)) {
        return false;
    }
    m_minBandwidthMhz = minMhz;
    m_maxBandwidthMhz = maxMhz;
    emit bandwidthLimitsChanged(m_minBandwidthMhz, m_maxBandwidthMhz);
    return true;
}

void PanadapterModel::setRfGain(int gain)
{
    if (gain != m_rfGain) {
        m_rfGain = gain;
        emit rfGainChanged(m_rfGain);
    }
}

void PanadapterModel::setRxAntenna(const QString& ant)
{
    if (ant != m_rxAntenna) {
        m_rxAntenna = ant;
        emit rxAntennaChanged(m_rxAntenna);
    }
}

void PanadapterModel::setWide(bool wide)
{
    if (wide == m_wideActive)
        return;
    m_wideActive = wide;
    emit wideChanged(m_wideActive);
}

void PanadapterModel::setAntList(const QStringList& ants)
{
    if (ants != m_antList) {
        m_antList = ants;
        emit antListChanged(m_antList);
    }
}

void PanadapterModel::setWaterfallLineDuration(int rate)
{
    // `rate` is the 1..100 waterfall RATE, low slow / high fast — the method and
    // field keep Flex's `line_duration` wire name because that is what arrives
    // on the wire, but the VALUE is not milliseconds. See core/WaterfallRate.h.
    // (#4606)
    //
    // PerfTelemetry is fed every report (even when unchanged), and
    // waterfallLineDurationReported likewise always fires; the change-gated
    // signal is waterfallLineDurationChanged. Semantics preserved verbatim from
    // a radio status observation.
    PerfTelemetry::instance().setWaterfallRate(rate);
    if (rate != m_waterfallLineDuration) {
        m_waterfallLineDuration = rate;
        emit waterfallLineDurationChanged(m_waterfallLineDuration);
    }
    emit waterfallLineDurationReported(rate);
}

void PanadapterModel::setDisplayRates(int fps, int wfRate)
{
    // fps reuses the same reported/changed pair the Flex status path emits, so
    // the widget's existing wiring picks it up with no special case.
    if (fps > 0) {
        if (fps != m_fps) {
            m_fps = fps;
            emit fpsChanged(m_fps);
        }
        emit fpsReported(fps);
    }
    if (wfRate > 0) {
        setWaterfallLineDuration(wfRate);
    }
}

void PanadapterModel::setLocalAverage(int average)
{
    if (average < 0 || average > 100) {
        return;
    }
    // Authoritative, so the request flag is CLEARED rather than set: there is
    // no echo coming and this value is not provisional.
    m_averageIsRequest = false;
    if (m_average != average) {
        m_average = average;
        emit averageChanged(average);
    }
    // Reported as well as Changed, like setDisplayRates' fps half, because the
    // restore path reads the reported value when a pan is rebuilt.
    emit averageReported(average);
}

void PanadapterModel::setLocalWeightedAverage(bool weighted)
{
    m_weightedAverageKnown = true;
    if (weighted != m_weightedAverage) {
        m_weightedAverage = weighted;
        emit weightedAverageChanged(m_weightedAverage);
    }
    emit weightedAverageReported(weighted);
}

void PanadapterModel::setRequestedFftSettings(int average, int fps)
{
    // Only call after dispatch. Do not emit *Reported, persist, or schedule a
    // replay. The next valid radio publication always supersedes this intent,
    // including a publication equal to the value before our request.
    const bool provenanceChanged = (average >= 0 && average <= 100 && !m_averageIsRequest)
        || (fps > 0 && fps <= 100 && !m_fpsIsRequest);
    if (average >= 0 && average <= 100) {
        m_averageIsRequest = true;
        if (m_average != average) {
            m_average = average;
            emit averageChanged(average);
        }
    }
    if (fps > 0 && fps <= 100) {
        m_fpsIsRequest = true;
        if (m_fps != fps) {
            m_fps = fps;
            emit fpsChanged(fps);
        }
    }
    if (provenanceChanged) {
        emit fftProvenanceChanged();
    }
}

bool PanadapterModel::applyDisplayValues(const DisplayValues& v, const std::function<bool()>& current)
{
    const QPointer<PanadapterModel> self(this);
    const auto alive = [&] { return self && (!current || current()); };
    if (!alive()) { return false; }
    if (v.centerMhz || v.bandwidthMhz) {
        recordGeometryObservation(v.centerMhz.value_or(-1), v.bandwidthMhz.value_or(-1));
        if (!alive()) { return false; }
        setCenterBandwidth(v.centerMhz.value_or(-1), v.bandwidthMhz.value_or(-1));
        if (!alive()) { return false; }
    }
    if (v.minDbm || v.maxDbm) {
        const double absent = std::numeric_limits<double>::quiet_NaN();
        setRange(v.minDbm.value_or(absent), v.maxDbm.value_or(absent));
        if (!alive()) { return false; }
    }
    if (v.rfGain) { setRfGain(*v.rfGain); if (!alive()) { return false; } }
    if (v.rxAntenna) { setRxAntenna(*v.rxAntenna); if (!alive()) { return false; } }
    if (v.antennas) { setAntList(*v.antennas); if (!alive()) { return false; } }
    if (v.wide) { setWide(*v.wide); if (!alive()) { return false; } }
    if (v.loopA || v.loopB) {
        const bool beforeA = m_loopA;
        const bool beforeB = m_loopB;
        if (v.loopA) { m_loopA = *v.loopA; if (m_loopA) { m_loopB = false; } }
        if (v.loopB) { m_loopB = *v.loopB; if (m_loopB) { m_loopA = false; } }
        if (beforeA != m_loopA || beforeB != m_loopB) {
            emit loopChanged(m_loopA, m_loopB);
            if (!alive()) { return false; }
        }
    }
    if (v.fps) {
        m_radioReportedFps = *v.fps;
        ++m_fftObservationRevision;
        m_fpsIsRequest = false;
        if (m_fps != *v.fps) {
            m_fps = *v.fps;
            emit fpsChanged(m_fps);
            if (!alive()) { return false; }
        }
        emit fpsReported(*v.fps);
        if (!alive()) { return false; }
    }
    if (v.average) {
        m_radioReportedAverage = *v.average;
        ++m_fftObservationRevision;
        m_averageIsRequest = false;
        if (m_average != *v.average) {
            m_average = *v.average;
            emit averageChanged(m_average);
            if (!alive()) { return false; }
        }
        emit averageReported(*v.average);
        if (!alive()) { return false; }
    }
    if (v.weightedAverage) {
        m_weightedAverageKnown = true;
        if (m_weightedAverage != *v.weightedAverage) {
            m_weightedAverage = *v.weightedAverage;
            emit weightedAverageChanged(m_weightedAverage);
            if (!alive()) { return false; }
        }
        emit weightedAverageReported(m_weightedAverage);
        if (!alive()) { return false; }
    }
    if (v.preamp) { m_preamp = *v.preamp; }
    if (v.daxIqChannel && m_daxiqChannel != *v.daxIqChannel) {
        m_daxiqChannel = *v.daxIqChannel;
        emit daxiqChannelChanged(m_daxiqChannel);
        if (!alive()) { return false; }
    }
    if (v.bandZoom && m_bandZoomOn != *v.bandZoom) {
        m_bandZoomOn = *v.bandZoom;
        emit bandZoomChanged(m_bandZoomOn);
        if (!alive()) { return false; }
    }
    if (v.segmentZoom && m_segmentZoomOn != *v.segmentZoom) {
        m_segmentZoomOn = *v.segmentZoom;
        emit segmentZoomChanged(m_segmentZoomOn);
        if (!alive()) { return false; }
    }
    if (v.noiseFloorRange && m_noiseFloorRange != *v.noiseFloorRange) {
        m_noiseFloorRange = *v.noiseFloorRange;
        emit noiseFloorRangeChanged(*m_noiseFloorRange);
        if (!alive()) { return false; }
    }
    const bool oldWnb = m_wnbActive;
    const int oldLevel = m_wnbLevel;
    const bool oldUpdating = m_wnbUpdating;
    if (v.wnb) { m_wnbActive = *v.wnb; }
    if (v.wnbLevel) { m_wnbLevel = *v.wnbLevel; }
    if (v.wnbUpdating) { m_wnbUpdating = *v.wnbUpdating; }
    if (oldWnb != m_wnbActive || oldLevel != m_wnbLevel || oldUpdating != m_wnbUpdating) {
        emit wnbChanged(m_wnbActive, m_wnbLevel);
        if (!alive()) { return false; }
        emit wnbStateChanged(m_wnbActive, m_wnbLevel, m_wnbUpdating);
        if (!alive()) { return false; }
    }
    if (v.waterfallRate) {
        PerfTelemetry::instance().setWaterfallRate(*v.waterfallRate);
        if (m_waterfallLineDuration != *v.waterfallRate) {
            m_waterfallLineDuration = *v.waterfallRate;
            emit waterfallLineDurationChanged(m_waterfallLineDuration);
            if (!alive()) { return false; }
        }
        emit waterfallLineDurationReported(m_waterfallLineDuration);
        if (!alive()) { return false; }
    }
    return true;
}

} // namespace AetherSDR
