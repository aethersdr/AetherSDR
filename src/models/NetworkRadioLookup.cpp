#include "NetworkRadioLookup.h"

#include <QAbstractSocket>
#include <QPointer>

#include <algorithm>
#include <utility>

namespace AetherSDR {
NetworkRadioLookup::NetworkRadioLookup(QObject* parent)
    : NetworkRadioLookup([](const QString& host, QObject* context, std::function<void(const QHostInfo&)> callback) {
        return QHostInfo::lookupHost(host, context, std::move(callback));
    }, [](int id) { QHostInfo::abortHostLookup(id); }, 10000, parent)
{}

NetworkRadioLookup::NetworkRadioLookup(Resolver resolver, Canceller canceller, int timeoutMs, QObject* parent)
    : QObject(parent), m_resolver(std::move(resolver)), m_canceller(std::move(canceller)),
      m_timeoutMs(std::max(1, timeoutMs))
{
    m_timer.setParent(this);
    m_timer.setSingleShot(true);
}

NetworkRadioLookup::~NetworkRadioLookup() { cancel(); }

void NetworkRadioLookup::cancel()
{
    ++m_generation;
    m_active = false;
    m_timer.stop();
    disconnect(m_timeoutConnection);
    m_completion = {};
    const int id = std::exchange(m_lookupId, -1);
    const Canceller abort = m_canceller;
    if (id >= 0 && abort) { abort(id); }
}

void NetworkRadioLookup::start(const QString& host, Completion completion)
{
    const QPointer<NetworkRadioLookup> guard(this);
    const quint64 entryGeneration = m_generation;
    cancel();
    if (!guard || m_generation != entryGeneration + 1) { return; }
    const quint64 generation = m_generation;
    m_active = true;
    m_completion = std::move(completion);
    const QString name = host.trimmed();
    if (name.isEmpty() || name.size() > 253 || !m_resolver) {
        finish(generation, {{}, Error::InvalidHost});
        return;
    }
    const QHostAddress numeric(name);
    if (!numeric.isNull()) {
        finish(generation, {numeric.protocol() == QAbstractSocket::IPv4Protocol ? numeric : QHostAddress{},
            numeric.protocol() == QAbstractSocket::IPv4Protocol ? Error::None : Error::NoIpv4Address});
        return;
    }
    m_timeoutConnection = connect(&m_timer, &QTimer::timeout, this, [this, generation] {
        finish(generation, {{}, Error::TimedOut}, true);
    });
    m_timer.start(m_timeoutMs);
    const Resolver resolve = m_resolver;
    const Canceller abort = m_canceller;
    const int id = resolve(name, this, [guard, generation](const QHostInfo& info) {
        if (!guard || !guard->m_active || generation != guard->m_generation) { return; }
        QHostAddress address;
        if (info.error() == QHostInfo::NoError) {
            for (const QHostAddress& candidate : info.addresses()) {
                if (candidate.protocol() == QAbstractSocket::IPv4Protocol) { address = candidate; break; }
            }
        }
        guard->finish(generation, {address, address.isNull() ? Error::NoIpv4Address : Error::None});
    });
    if (guard && generation == m_generation && m_active) {
        if (id >= 0) { m_lookupId = id; }
        else { finish(generation, {{}, Error::NoIpv4Address}); }
    } else if (id >= 0 && abort) {
        abort(id);
    }
}

void NetworkRadioLookup::finish(quint64 generation, Result result, bool abortLookup)
{
    if (!m_active || generation != m_generation) { return; }
    m_active = false;
    m_timer.stop();
    disconnect(m_timeoutConnection);
    const int id = std::exchange(m_lookupId, -1);
    Completion completed = std::move(m_completion);
    const Canceller abort = m_canceller;
    const QPointer<NetworkRadioLookup> guard(this);
    if (abortLookup && id >= 0 && abort) { abort(id); }
    if (guard && completed) { completed(std::move(result)); }
}
} // namespace AetherSDR
