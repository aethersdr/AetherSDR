#include "core/SimIqProvider.h"

#include <cmath>
#include <utility>

namespace AetherSDR {

namespace {
// File-local rather than the M_PI macro: that one is absent on MSVC without
// _USE_MATH_DEFINES, and check-windows is where we would find out.
constexpr double kPi = 3.14159265358979323846;
}  // namespace

SimIqProvider::SimIqProvider(QObject* parent) : IqProvider(parent) {}

void SimIqProvider::setEndpoints(const QVector<EndpointSpec>& eps)
{
    QVector<QString> vanished;
    for (const EndpointSpec& old : m_endpoints) {
        bool stillThere = false;
        for (const EndpointSpec& e : eps) {
            if (e.id == old.id) { stillThere = true; break; }
        }
        if (!stillThere)
            vanished.append(old.id);
    }
    m_endpoints = eps;
    // Same rule every real adapter follows: a receiver that went away ends its
    // leases. It is never rebound to whatever now occupies its slot.
    for (const QString& id : vanished)
        invalidateEndpoint(id, IqRefusal::UnknownEndpoint);
    for (const EndpointSpec& e : m_endpoints)
        noteAchieved(e.id, m_achievedHz, e.centerHz);
    emit endpointsChanged();
}

void SimIqProvider::removeEndpoint(const QString& id)
{
    QVector<EndpointSpec> next;
    for (const EndpointSpec& e : m_endpoints) {
        if (e.id != id)
            next.append(e);
    }
    setEndpoints(next);
}

void SimIqProvider::setConnected(bool on)
{
    if (m_connected == on)
        return;
    m_connected = on;
    if (!on)
        invalidateAllEndpoints(IqRefusal::NotConnected);
    emit endpointsChanged();
}

void SimIqProvider::setCapacity(int maxConcurrentEndpoints)
{
    m_capacity = maxConcurrentEndpoints;
}

void SimIqProvider::setRatePolicy(const QVector<int>& supported, int achievedHz,
                                  IqRateTopology topology, bool radioWide)
{
    m_supportedRates = supported;
    m_achievedHz     = achievedHz;
    m_topology       = topology;
    m_radioWide      = radioWide;
    for (const EndpointSpec& e : m_endpoints)
        noteAchieved(e.id, m_achievedHz, e.centerHz);
}

IqProviderCaps SimIqProvider::capabilities() const
{
    IqProviderCaps caps;
    caps.maxConcurrentEndpoints = m_capacity;
    caps.supportedRatesHz       = m_supportedRates;
    caps.rateTopology           = m_topology;
    caps.rateChangeIsRadioWide  = m_radioWide;
    return caps;
}

QVector<IqEndpoint> SimIqProvider::endpoints() const
{
    QVector<IqEndpoint> out;
    out.reserve(m_endpoints.size());
    for (const EndpointSpec& e : m_endpoints) {
        IqEndpoint ep;
        ep.id             = e.id;
        ep.label          = e.id;
        ep.live           = m_connected;
        ep.achievedRateHz = m_achievedHz;
        ep.centerHz       = e.centerHz;
        out.append(ep);
    }
    return out;
}

const SimIqProvider::EndpointSpec* SimIqProvider::find(const QString& id) const
{
    for (const EndpointSpec& e : m_endpoints) {
        if (e.id == id)
            return &e;
    }
    return nullptr;
}

IqLease SimIqProvider::openEndpoint(const QString& endpointId, int requestedRateHz)
{
    if (!m_connected)
        return IqLease::refuse(IqRefusal::NotConnected);
    const EndpointSpec* e = find(endpointId);
    if (!e)
        return IqLease::refuse(IqRefusal::UnknownEndpoint);

    // Capacity counted in OPEN ENDPOINTS, not subscribers: two clients on one
    // receiver cost one endpoint, which is the whole point of the lease layer.
    int open = 0;
    for (const EndpointSpec& s : m_endpoints) {
        if (isLeased(s.id))
            ++open;
    }
    if (open >= m_capacity)
        return IqLease::refuse(IqRefusal::CapacityExceeded);

    ++m_opens;
    IqLease l = adjustRate(endpointId, requestedRateHz);
    l.granted    = true;
    l.endpointId = endpointId;
    l.centerHz   = e->centerHz;
    return l;
}

void SimIqProvider::closeEndpoint(const QString& endpointId)
{
    Q_UNUSED(endpointId);
    ++m_closes;
}

IqLease SimIqProvider::adjustRate(const QString& endpointId, int requestedRateHz)
{
    const EndpointSpec* e = find(endpointId);
    if (!e)
        return IqLease::refuse(IqRefusal::UnknownEndpoint);

    if (requestedRateHz > 0 && m_topology == IqRateTopology::PerStream
        && m_supportedRates.contains(requestedRateHz)) {
        m_achievedHz = requestedRateHz;
    }
    // Everything else — an unsupported rate, a shared-rate radio, a fixed one —
    // reports what is running. Never the request.

    IqLease l;
    l.granted        = true;
    l.endpointId     = endpointId;
    l.achievedRateHz = m_achievedHz;
    l.centerHz       = e->centerHz;
    return l;
}

void SimIqProvider::emitTone(const QString& endpointId, double offsetHz, int sampleCount)
{
    const EndpointSpec* e = find(endpointId);
    if (!e || sampleCount <= 0)
        return;
    std::vector<IqSample> samples(static_cast<std::size_t>(sampleCount));
    const double w = 2.0 * kPi * offsetHz / static_cast<double>(m_achievedHz);
    for (int n = 0; n < sampleCount; ++n) {
        const double phase = w * n;
        samples[static_cast<std::size_t>(n)] =
            IqSample(static_cast<float>(std::cos(phase)),
                     static_cast<float>(std::sin(phase)));
    }
    publishBlock(endpointId, m_achievedHz, e->centerHz, std::move(samples));
}

void SimIqProvider::emitSamples(const QString& endpointId, std::vector<IqSample> samples)
{
    const EndpointSpec* e = find(endpointId);
    if (!e)
        return;
    publishBlock(endpointId, m_achievedHz, e->centerHz, std::move(samples));
}

}  // namespace AetherSDR
