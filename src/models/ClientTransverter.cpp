#include "ClientTransverter.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR {

bool ClientTransverter::isUsable() const
{
    return enabled && std::isfinite(ifLowMhz) && std::isfinite(ifHighMhz)
        && std::isfinite(rxLoMhz) && std::isfinite(txLoMhz)
        && ifLowMhz > 0.0 && ifHighMhz > ifLowMhz && rxLoMhz > 0.0
        && txLoMhz >= 0.0;
}

QJsonObject ClientTransverter::toJson() const
{
    return {
        {QStringLiteral("name"), name},
        {QStringLiteral("ifLowMhz"), ifLowMhz},
        {QStringLiteral("ifHighMhz"), ifHighMhz},
        {QStringLiteral("rxLoMhz"), rxLoMhz},
        {QStringLiteral("txLoMhz"), txLoMhz},
        {QStringLiteral("enabled"), enabled},
    };
}

ClientTransverter ClientTransverter::fromJson(const QJsonObject& o)
{
    ClientTransverter t;
    t.name = o.value(QStringLiteral("name")).toString();
    t.ifLowMhz = o.value(QStringLiteral("ifLowMhz")).toDouble();
    t.ifHighMhz = o.value(QStringLiteral("ifHighMhz")).toDouble();
    t.rxLoMhz = o.value(QStringLiteral("rxLoMhz")).toDouble();
    t.txLoMhz = o.value(QStringLiteral("txLoMhz")).toDouble();
    t.enabled = o.value(QStringLiteral("enabled")).toBool(true);
    return t;
}

void ClientTransverterSet::setTransverters(const QVector<ClientTransverter>& list)
{
    m_list = list;
    if (m_active >= m_list.size() || (m_active >= 0 && !m_list[m_active].isUsable())) {
        m_active = -1;
    }
}

int ClientTransverterSet::indexForRf(double mhz) const
{
    for (int i = 0; i < m_list.size(); ++i) {
        const ClientTransverter& t = m_list[i];
        if (t.isUsable() && mhz >= t.rfLowMhz() && mhz <= t.rfHighMhz()) {
            return i;
        }
    }
    return -1;
}

void ClientTransverterSet::setNativeRangeMhz(double minMhz, double maxMhz)
{
    m_nativeMinMhz = minMhz;
    m_nativeMaxMhz = maxMhz;
}

bool ClientTransverterSet::isNative(double mhz) const
{
    // Unknown range: every frequency counts as native, the pre-transverter rule.
    return m_nativeMaxMhz <= m_nativeMinMhz
        || (mhz >= m_nativeMinMhz && mhz <= m_nativeMaxMhz);
}

double ClientTransverterSet::toRadioMhz(double requestedMhz)
{
    if (const int idx = indexForRf(requestedMhz); idx >= 0) {
        m_active = idx;
        return requestedMhz - m_list[idx].rxLoMhz;
    }
    if (m_active >= 0 && !isNative(requestedMhz)) {
        const ClientTransverter& t = m_list[m_active];
        const double held = std::clamp(requestedMhz, t.rfLowMhz(), t.rfHighMhz());
        return held - t.rxLoMhz;
    }
    m_active = -1;
    return requestedMhz;
}

double ClientTransverterSet::toDisplayMhz(double radioMhz)
{
    if (m_active < 0) {
        return radioMhz;
    }
    const ClientTransverter& t = m_list[m_active];
    if (radioMhz < t.ifLowMhz || radioMhz > t.ifHighMhz) {
        m_active = -1;
        return radioMhz;
    }
    return radioMhz + t.rxLoMhz;
}

double ClientTransverterSet::shiftToDisplayMhz(double radioMhz) const
{
    return m_active >= 0 ? radioMhz + m_list[m_active].rxLoMhz : radioMhz;
}

double ClientTransverterSet::shiftToRadioMhz(double displayMhz) const
{
    return m_active >= 0 ? displayMhz - m_list[m_active].rxLoMhz : displayMhz;
}

double ClientTransverterSet::toDisplayTxMhz(double radioMhz) const
{
    return m_active >= 0 ? radioMhz + m_list[m_active].txLo() : radioMhz;
}

const ClientTransverter* ClientTransverterSet::active() const
{
    return m_active >= 0 ? &m_list[m_active] : nullptr;
}

void ClientTransverterSet::setActiveIndex(int index)
{
    m_active = (index >= 0 && index < m_list.size() && m_list[index].isUsable()) ? index : -1;
}

} // namespace AetherSDR
