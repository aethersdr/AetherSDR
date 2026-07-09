#include "PanadapterModel.h"
#include "core/PerfTelemetry.h"
#include <QDebug>
#include <algorithm>
#include <cmath>

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
    m_clientHandle = h;
    m_ownerHandle = parseHandleHex(h);
}

bool PanadapterModel::ownedByClient(quint32 handle) const
{
    // An unknown owner is treated as ours: the radio hasn't told us
    // otherwise, and failing open here would break every dBm command.
    // Fail-open is ONLY safe for gating our own outbound commands — evidence
    // against other clients must use ownerHandle() and fail closed (#3977).
    return m_ownerHandle == 0 || m_ownerHandle == handle;
}

void PanadapterModel::setRfGainInfo(int low, int high, int step)
{
    m_rfGainLow = low;
    m_rfGainHigh = high;
    m_rfGainStep = step;
    emit rfGainInfoChanged(low, high, step);
}

void PanadapterModel::setCenterBandwidth(double centerMhz, double bandwidthMhz)
{
    bool changed = false;
    if (centerMhz >= 0.0 && centerMhz != m_centerMhz) {
        m_centerMhz = centerMhz;
        changed = true;
    }
    if (bandwidthMhz >= 0.0 && bandwidthMhz != m_bandwidthMhz) {
        m_bandwidthMhz = bandwidthMhz;
        changed = true;
    }
    if (changed) {
        emit infoChanged(m_centerMhz, m_bandwidthMhz);
    }
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

void PanadapterModel::applyWnbExtension(const QVariantMap& fields)
{
    bool dirty = false;
    if (fields.contains(QStringLiteral("wnb"))) {
        const bool w = fields.value(QStringLiteral("wnb")).toBool();
        if (w != m_wnbActive) { m_wnbActive = w; dirty = true; }
    }
    if (fields.contains(QStringLiteral("wnb_level"))) {
        const int lvl = std::clamp(fields.value(QStringLiteral("wnb_level")).toInt(), 0, 100);
        if (lvl != m_wnbLevel) { m_wnbLevel = lvl; dirty = true; }
    }
    if (fields.contains(QStringLiteral("wnb_updating"))) {
        const bool u = fields.value(QStringLiteral("wnb_updating")).toBool();
        if (u != m_wnbUpdating) { m_wnbUpdating = u; dirty = true; }
    }
    if (dirty) {
        emit wnbChanged(m_wnbActive, m_wnbLevel);
        emit wnbStateChanged(m_wnbActive, m_wnbLevel, m_wnbUpdating);
    }
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

void PanadapterModel::setAntList(const QStringList& ants)
{
    if (ants != m_antList) {
        m_antList = ants;
        emit antListChanged(m_antList);
    }
}

void PanadapterModel::setWaterfallLineDuration(int ms)
{
    // PerfTelemetry is fed every report (even when unchanged), and
    // waterfallLineDurationReported likewise always fires; the change-gated
    // signal is waterfallLineDurationChanged. Semantics preserved verbatim from
    // the old applyWaterfallStatus.
    PerfTelemetry::instance().setWaterfallLineDurationMs(ms);
    if (ms != m_waterfallLineDuration) {
        m_waterfallLineDuration = ms;
        emit waterfallLineDurationChanged(m_waterfallLineDuration);
    }
    emit waterfallLineDurationReported(ms);
}

void PanadapterModel::applyStateExtension(const QVariantMap& fields)
{
    // The Flex-specific display-pan fields, applied from the backend's
    // namespaced extensionStatus("flex","panState",…). Each key applies only
    // when present, with the exact per-field semantics the old applyPanStatus
    // had (aetherd RFC 2.3 — the decode lives in FlexBackend, not here).
    if (fields.contains(QStringLiteral("wide"))) {
        const bool wide = fields.value(QStringLiteral("wide")).toInt() != 0;
        if (wide != m_wideActive) {
            m_wideActive = wide;
            emit wideChanged(m_wideActive);
        }
    }
    if (fields.contains(QStringLiteral("loopa"))
        || fields.contains(QStringLiteral("loopb"))) {
        bool changed = false;
        if (fields.contains(QStringLiteral("loopa"))) {
            const bool loopA = fields.value(QStringLiteral("loopa")).toInt() != 0;
            if (loopA != m_loopA) { m_loopA = loopA; changed = true; }
            if (loopA && m_loopB) { m_loopB = false; changed = true; }
        }
        if (fields.contains(QStringLiteral("loopb"))) {
            const bool loopB = fields.value(QStringLiteral("loopb")).toInt() != 0;
            if (loopB != m_loopB) { m_loopB = loopB; changed = true; }
            if (loopB && m_loopA) { m_loopA = false; changed = true; }
        }
        if (changed) {
            emit loopChanged(m_loopA, m_loopB);
        }
    }
    if (fields.contains(QStringLiteral("fps"))) {
        bool ok = false;
        const int fps = fields.value(QStringLiteral("fps")).toInt(&ok);
        if (ok) {
            if (fps != m_fps) {
                m_fps = fps;
                emit fpsChanged(m_fps);
            }
            emit fpsReported(fps);
        }
    }
    if (fields.contains(QStringLiteral("pre"))) {
        const QString pre = fields.value(QStringLiteral("pre")).toString();
        // Preamp is internal state only — no UI listeners, no emit (#1498).
        if (pre != m_preamp) { m_preamp = pre; }
    }
    if (fields.contains(QStringLiteral("daxiq_channel"))) {
        const int ch = fields.value(QStringLiteral("daxiq_channel")).toInt();
        if (ch != m_daxiqChannel) {
            m_daxiqChannel = ch;
            emit daxiqChannelChanged(ch);
        }
    }
    // Band / segment zoom (#4057). Mirror FlexLib's parse exactly (Panadapter.cs
    // 933-947/1159-1173): uint parse, values > 1 invalid → skip, no local mutual
    // exclusion — the radio clears the sibling flag itself and broadcasts both
    // transitions. Malformed values are ignored, not applied as 0, matching the
    // wnb_level guard above (Principle VII).
    if (fields.contains(QStringLiteral("band_zoom"))) {
        bool ok = false;
        const uint v = fields.value(QStringLiteral("band_zoom")).toString().toUInt(&ok);
        if (ok && v <= 1) {
            const bool on = (v == 1);
            if (on != m_bandZoomOn) {
                m_bandZoomOn = on;
                emit bandZoomChanged(on);
            }
        }
    }
    if (fields.contains(QStringLiteral("segment_zoom"))) {
        bool ok = false;
        const uint v = fields.value(QStringLiteral("segment_zoom")).toString().toUInt(&ok);
        if (ok && v <= 1) {
            const bool on = (v == 1);
            if (on != m_segmentZoomOn) {
                m_segmentZoomOn = on;
                emit segmentZoomChanged(on);
            }
        }
    }
    // #3977: ownership is radio-authoritative. When another session reclaims
    // this pan (MultiFlex reconnect), the radio broadcasts the new
    // client_handle; tracking it lets a superseded session stop adjusting a pan
    // it no longer owns. Semantics preserved verbatim (parsed != 0 && changed).
    if (fields.contains(QStringLiteral("client_handle"))) {
        const quint32 parsed =
            parseHandleHex(fields.value(QStringLiteral("client_handle")).toString());
        if (parsed != 0 && parsed != m_ownerHandle) {
            m_ownerHandle = parsed;
            m_clientHandle = QString::number(parsed, 16);
        }
    }
    if (fields.contains(QStringLiteral("waterfall"))) {
        setWaterfallId(fields.value(QStringLiteral("waterfall")).toString());
    }
}

} // namespace AetherSDR
