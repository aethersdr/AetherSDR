#include "PanadapterModel.h"
#include "core/PerfTelemetry.h"
#include <QDebug>

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

void PanadapterModel::setClientHandle(const QString& h)
{
    m_clientHandle = h;
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
    if (kvs.contains("wnb")) {
        bool w = kvs["wnb"].toInt() != 0;
        int lvl = kvs.value("wnb_level", QString::number(m_wnbLevel)).toInt();
        if (w != m_wnbActive || lvl != m_wnbLevel) {
            m_wnbActive = w;
            m_wnbLevel = lvl;
            emit wnbChanged(m_wnbActive, m_wnbLevel);
        }
    }
    if (kvs.contains("wide")) {
        bool wide = kvs["wide"].toInt() != 0;
        if (wide != m_wideActive) {
            m_wideActive = wide;
            emit wideChanged(m_wideActive);
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
