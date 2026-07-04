#include "PanadapterModel.h"
#include "core/PerfTelemetry.h"
#include <QDebug>
#include <algorithm>

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

void PanadapterModel::applyPanStatus(const QMap<QString, QString>& kvs)
{
    bool infoChanged = false;
    bool levelChanged = false;

    // #3977: ownership is radio-authoritative. When another session reclaims
    // this pan (MultiFlex reconnect), the radio broadcasts the new
    // client_handle; tracking it here lets a superseded session stop
    // adjusting a pan it no longer owns.
    if (kvs.contains("client_handle")) {
        const quint32 parsed = parseHandleHex(kvs.value("client_handle"));
        if (parsed != 0 && parsed != m_ownerHandle) {
            m_ownerHandle = parsed;
            m_clientHandle = QString::number(parsed, 16);
        }
    }

    if (kvs.contains("center")) {
        double c = kvs["center"].toDouble();
        if (c != m_centerMhz) { m_centerMhz = c; infoChanged = true; }
    }
    if (kvs.contains("bandwidth")) {
        double b = kvs["bandwidth"].toDouble();
        if (b != m_bandwidthMhz) { m_bandwidthMhz = b; infoChanged = true; }
    }
    if (kvs.contains("min_dbm")) {
        float v = kvs["min_dbm"].toFloat();
        if (v != m_minDbm) { m_minDbm = v; levelChanged = true; }
    }
    if (kvs.contains("max_dbm")) {
        float v = kvs["max_dbm"].toFloat();
        if (v != m_maxDbm) { m_maxDbm = v; levelChanged = true; }
    }
    if (kvs.contains("rfgain")) {
        int g = kvs["rfgain"].toInt();
        if (g != m_rfGain) {
            m_rfGain = g;
            emit rfGainChanged(m_rfGain);
        }
    }
    if (kvs.contains("pre")) {
        QString pre = kvs["pre"];
        if (pre != m_preamp) {
            // Preamp is internal state only — no UI listeners, no emit.
            m_preamp = pre;
        }
    }
    // FlexLib v4.2.18 exposes wnb_updating on display pan status while the
    // radio normalizes the SCU-level WNB threshold; keep it distinct from
    // the per-pan WNB enable flag.
    bool wnbStateDirty = false;
    if (kvs.contains("wnb")) {
        const bool w = kvs["wnb"].toInt() != 0;
        if (w != m_wnbActive) {
            m_wnbActive = w;
            wnbStateDirty = true;
        }
    }
    if (kvs.contains("wnb_level")) {
        bool ok = false;
        const int lvl = std::clamp(kvs["wnb_level"].toInt(&ok), 0, 100);
        if (ok && lvl != m_wnbLevel) {
            m_wnbLevel = lvl;
            wnbStateDirty = true;
        }
    }
    if (kvs.contains("wnb_updating")) {
        const bool updating = kvs["wnb_updating"].toInt() != 0;
        if (updating != m_wnbUpdating) {
            m_wnbUpdating = updating;
            wnbStateDirty = true;
        }
    }
    if (wnbStateDirty) {
        emit wnbChanged(m_wnbActive, m_wnbLevel);
        emit wnbStateChanged(m_wnbActive, m_wnbLevel, m_wnbUpdating);
    }
    if (kvs.contains("wide")) {
        bool wide = kvs["wide"].toInt() != 0;
        if (wide != m_wideActive) {
            m_wideActive = wide;
            emit wideChanged(m_wideActive);
        }
    }
    if (kvs.contains("loopa") || kvs.contains("loopb")) {
        bool changed = false;
        if (kvs.contains("loopa")) {
            const bool loopA = kvs["loopa"].toInt() != 0;
            if (loopA != m_loopA) {
                m_loopA = loopA;
                changed = true;
            }
            if (loopA && m_loopB) {
                m_loopB = false;
                changed = true;
            }
        }
        if (kvs.contains("loopb")) {
            const bool loopB = kvs["loopb"].toInt() != 0;
            if (loopB != m_loopB) {
                m_loopB = loopB;
                changed = true;
            }
            if (loopB && m_loopA) {
                m_loopA = false;
                changed = true;
            }
        }
        if (changed) {
            emit loopChanged(m_loopA, m_loopB);
        }
    }
    if (kvs.contains("fps")) {
        bool ok = false;
        const int fps = kvs["fps"].toInt(&ok);
        if (ok) {
            if (fps != m_fps) {
                m_fps = fps;
                emit fpsChanged(m_fps);
            }
            emit fpsReported(fps);
        }
    }
    if (kvs.contains("ant_list")) {
        QStringList ants = kvs["ant_list"].split(',', Qt::SkipEmptyParts);
        if (ants != m_antList) {
            m_antList = ants;
            emit antListChanged(m_antList);
        }
    }
    if (kvs.contains("rxant")) {
        const QString ant = kvs["rxant"];
        if (ant != m_rxAntenna) {
            m_rxAntenna = ant;
            emit rxAntennaChanged(m_rxAntenna);
        }
    }
    if (kvs.contains("waterfall")) {
        setWaterfallId(kvs["waterfall"]);
    }
    if (kvs.contains("daxiq_channel")) {
        int ch = kvs["daxiq_channel"].toInt();
        if (ch != m_daxiqChannel) {
            m_daxiqChannel = ch;
            emit daxiqChannelChanged(ch);
        }
    }

    if (infoChanged)
        emit this->infoChanged(m_centerMhz, m_bandwidthMhz);
    if (levelChanged)
        emit this->levelChanged(m_minDbm, m_maxDbm);
}

void PanadapterModel::applyWaterfallStatus(const QMap<QString, QString>& kvs)
{
    if (kvs.contains("line_duration")) {
        bool ok = false;
        const int ms = kvs["line_duration"].toInt(&ok);
        if (ok) {
            PerfTelemetry::instance().setWaterfallLineDurationMs(ms);
            if (ms != m_waterfallLineDuration) {
                m_waterfallLineDuration = ms;
                emit waterfallLineDurationChanged(m_waterfallLineDuration);
            }
            emit waterfallLineDurationReported(ms);
        }
    }

    // Waterfall status shares center/bandwidth with pan — sync if present
    if (kvs.contains("center") || kvs.contains("bandwidth")) {
        bool changed = false;
        if (kvs.contains("center")) {
            double c = kvs["center"].toDouble();
            if (c != m_centerMhz) { m_centerMhz = c; changed = true; }
        }
        if (kvs.contains("bandwidth")) {
            double b = kvs["bandwidth"].toDouble();
            if (b != m_bandwidthMhz) { m_bandwidthMhz = b; changed = true; }
        }
        if (changed)
            emit infoChanged(m_centerMhz, m_bandwidthMhz);
    }
}

} // namespace AetherSDR
