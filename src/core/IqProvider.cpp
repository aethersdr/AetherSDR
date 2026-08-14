#include "core/IqProvider.h"

#include <QMetaObject>
#include <QVector>

#include <utility>

namespace AetherSDR {

QString iqRefusalText(IqRefusal r)
{
    switch (r) {
    case IqRefusal::None:             return QStringLiteral("granted");
    case IqRefusal::NoProvider:       return QStringLiteral("this radio exposes no IQ source");
    case IqRefusal::NotConnected:     return QStringLiteral("radio not connected");
    case IqRefusal::UnknownEndpoint:  return QStringLiteral("no such receiver");
    case IqRefusal::CapacityExceeded: return QStringLiteral("no IQ capacity left");
    case IqRefusal::RateUnavailable:  return QStringLiteral("requested sample rate unavailable");
    case IqRefusal::BackendRefused:   return QStringLiteral("refused by the radio backend");
    }
    return QStringLiteral("refused");
}

IqProvider::IqProvider(QObject* parent) : QObject(parent)
{
    // Registered here rather than at each connect site: a consumer on another
    // thread takes a queued connection, and an unregistered payload type is a
    // runtime "cannot queue arguments of type" that silently drops every block.
    static const bool once = []() {
        qRegisterMetaType<AetherSDR::IqBlockPtr>("AetherSDR::IqBlockPtr");
        qRegisterMetaType<AetherSDR::IqRefusal>("AetherSDR::IqRefusal");
        return true;
    }();
    Q_UNUSED(once);
}

IqProvider::~IqProvider() = default;

std::optional<IqEndpoint> IqProvider::endpoint(const QString& endpointId) const
{
    if (endpointId.isEmpty())
        return std::nullopt;
    const QVector<IqEndpoint> all = endpoints();
    for (const IqEndpoint& e : all) {
        if (e.id == endpointId)
            return e;
    }
    return std::nullopt;
}

IqLease IqProvider::acquire(const QString& endpointId, const void* subscriber,
                            int requestedRateHz)
{
    if (endpointId.isEmpty() || !subscriber)
        return IqLease::refuse(IqRefusal::UnknownEndpoint);

    bool alreadyOpen = false;
    {
        std::lock_guard<std::mutex> g(m_leaseMutex);
        const auto it = m_subscribers.constFind(endpointId);
        alreadyOpen = (it != m_subscribers.constEnd() && !it->isEmpty());
    }

    // The adapter hook runs OUTSIDE the lease lock: it talks to the radio, and
    // a backend that called back into isLeased()/noteAchieved() on its own
    // success path would deadlock against a lock held across the call.
    IqLease lease = alreadyOpen ? adjustRate(endpointId, requestedRateHz)
                                : openEndpoint(endpointId, requestedRateHz);
    if (!lease.granted)
        return lease;

    lease.endpointId = endpointId;
    {
        std::lock_guard<std::mutex> g(m_leaseMutex);
        m_subscribers[endpointId].insert(subscriber);
        Achieved& a = m_achieved[endpointId];
        a.rateHz   = lease.achievedRateHz;
        a.centerHz = lease.centerHz;
    }
    return lease;
}

void IqProvider::release(const QString& endpointId, const void* subscriber)
{
    if (endpointId.isEmpty() || !subscriber)
        return;

    bool last = false;
    {
        std::lock_guard<std::mutex> g(m_leaseMutex);
        auto it = m_subscribers.find(endpointId);
        if (it == m_subscribers.end())
            return;
        if (!it->remove(subscriber))
            return;                     // not ours to release
        if (it->isEmpty()) {
            m_subscribers.erase(it);
            last = true;
        }
    }
    if (!last)
        return;   // #4951: another client is still on this receiver

    // Nothing is subscribed any more, so anything already queued is destined
    // for nobody. Dropping it here is what makes teardown clean: no callback
    // can arrive after the last release.
    {
        std::lock_guard<std::mutex> g(m_queueMutex);
        auto q = m_queues.find(endpointId);
        if (q != m_queues.end())
            q->blocks.clear();
    }
    closeEndpoint(endpointId);
}

void IqProvider::releaseAll(const void* subscriber)
{
    if (!subscriber)
        return;
    QVector<QString> mine;
    {
        std::lock_guard<std::mutex> g(m_leaseMutex);
        for (auto it = m_subscribers.constBegin(); it != m_subscribers.constEnd(); ++it) {
            if (it->contains(subscriber))
                mine.append(it.key());
        }
    }
    for (const QString& id : mine)
        release(id, subscriber);
}

int IqProvider::subscriberCount(const QString& endpointId) const
{
    std::lock_guard<std::mutex> g(m_leaseMutex);
    const auto it = m_subscribers.constFind(endpointId);
    return it == m_subscribers.constEnd() ? 0 : static_cast<int>(it->size());
}

bool IqProvider::isLeased(const QString& endpointId) const
{
    return subscriberCount(endpointId) > 0;
}

IqLease IqProvider::requestRate(const QString& endpointId, int requestedRateHz)
{
    IqLease l = adjustRate(endpointId, requestedRateHz);
    if (l.granted) {
        l.endpointId = endpointId;
        noteAchieved(endpointId, l.achievedRateHz, l.centerHz);
    }
    return l;
}

IqLease IqProvider::adjustRate(const QString& endpointId, int requestedRateHz)
{
    Q_UNUSED(requestedRateHz);
    // The honest default: report what is actually running. An adapter that CAN
    // move the rate overrides this; one that cannot must never pretend it did,
    // because a skimmer that believes a rate it is not receiving decodes at the
    // wrong speed and reports nothing at all.
    IqLease l;
    l.endpointId = endpointId;
    std::lock_guard<std::mutex> g(m_leaseMutex);
    const auto it = m_achieved.constFind(endpointId);
    if (it == m_achieved.constEnd())
        return IqLease::refuse(IqRefusal::UnknownEndpoint);
    l.granted        = true;
    l.achievedRateHz = it->rateHz;
    l.centerHz       = it->centerHz;
    return l;
}

void IqProvider::noteAchieved(const QString& endpointId, int achievedRateHz,
                              long long centerHz)
{
    if (endpointId.isEmpty())
        return;
    bool moved = false;
    {
        std::lock_guard<std::mutex> g(m_leaseMutex);
        Achieved& a = m_achieved[endpointId];
        moved = (a.rateHz != achievedRateHz) || (a.centerHz != centerHz);
        a.rateHz   = achievedRateHz;
        a.centerHz = centerHz;
    }
    if (moved)
        emit endpointsChanged();
}

quint64 IqProvider::droppedBlocks(const QString& endpointId) const
{
    std::lock_guard<std::mutex> g(m_queueMutex);
    const auto it = m_queues.constFind(endpointId);
    return it == m_queues.constEnd() ? 0 : it->droppedTotal;
}

void IqProvider::publishBlock(const QString& endpointId, int sampleRateHz,
                              long long centerHz, std::vector<IqSample>&& samples)
{
    if (endpointId.isEmpty() || samples.empty())
        return;
    // The producer is a radio's network or real-time DSP thread. It must never
    // wait on the GUI, so the whole of this function is: take a short mutex,
    // move a buffer, maybe drop the oldest, post one wake.
    if (!isLeased(endpointId))
        return;

    bool needWake = false;
    {
        std::lock_guard<std::mutex> g(m_queueMutex);
        Queue& q = m_queues[endpointId];
        while (static_cast<int>(q.blocks.size()) >= kMaxQueuedBlocks) {
            q.blocks.pop_front();
            ++q.droppedTotal;
            ++q.droppedSinceDelivery;
        }
        auto block = std::make_shared<IqBlock>();
        block->endpointId    = endpointId;
        block->sampleRateHz  = sampleRateHz;
        block->centerHz      = centerHz;
        block->sequence      = q.sequence++;
        block->droppedBefore = q.droppedSinceDelivery;
        block->samples       = std::move(samples);
        q.droppedSinceDelivery = 0;
        q.blocks.push_back(std::move(block));
        needWake = true;
    }
    // One wake per burst: the flag is cleared by the drain, so a producer
    // running far ahead of the consumer posts a single event rather than one
    // per block. exchange() is what makes that safe across threads.
    if (needWake && !m_drainPosted.exchange(true)) {
        QMetaObject::invokeMethod(this, [this]() { drainQueues(); },
                                  Qt::QueuedConnection);
    }
}

void IqProvider::drainQueues()
{
    m_drainPosted.store(false);

    QHash<QString, std::deque<IqBlockPtr>> ready;
    {
        std::lock_guard<std::mutex> g(m_queueMutex);
        for (auto it = m_queues.begin(); it != m_queues.end(); ++it) {
            if (it->blocks.empty())
                continue;
            ready.insert(it.key(), std::move(it->blocks));
            it->blocks.clear();
        }
    }
    for (auto it = ready.constBegin(); it != ready.constEnd(); ++it) {
        // Re-checked after the swap: a release that landed between publish and
        // drain must not deliver one last block to a consumer that has gone.
        if (!isLeased(it.key()))
            continue;
        for (const IqBlockPtr& b : it.value())
            emit iqBlockReady(it.key(), b);
    }
}

void IqProvider::invalidateEndpoint(const QString& endpointId, IqRefusal reason,
                                    const QString& detail)
{
    bool had = false;
    {
        std::lock_guard<std::mutex> g(m_leaseMutex);
        had = m_subscribers.remove(endpointId) > 0;
        m_achieved.remove(endpointId);
    }
    {
        std::lock_guard<std::mutex> g(m_queueMutex);
        m_queues.remove(endpointId);
    }
    if (had) {
        emit leaseInvalidated(endpointId, reason,
                              detail.isEmpty() ? iqRefusalText(reason) : detail);
    }
}

void IqProvider::invalidateAllEndpoints(IqRefusal reason, const QString& detail)
{
    QVector<QString> ids;
    {
        std::lock_guard<std::mutex> g(m_leaseMutex);
        ids = QVector<QString>(m_subscribers.keyBegin(), m_subscribers.keyEnd());
    }
    for (const QString& id : ids)
        invalidateEndpoint(id, reason, detail);
}

}  // namespace AetherSDR
