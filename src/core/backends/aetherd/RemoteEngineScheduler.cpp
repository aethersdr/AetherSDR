#include "RemoteEngineScheduler.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR::aetherd {

namespace {
QString intentKey(const QString& resourceKey, const QString& method)
{
    return resourceKey + QLatin1Char('\n') + method;
}
} // namespace

RemoteEngineScheduler::Limits RemoteEngineScheduler::clientLimits(const QJsonObject& welcomeLimits)
{
    const double rate = welcomeLimits.value(QStringLiteral("requestsPerSecond")).toDouble();
    const double burst = welcomeLimits.value(QStringLiteral("requestBurst")).toDouble();
    if (!std::isfinite(rate) || !std::isfinite(burst) || rate <= 0.0 || burst <= 0.0) {
        return {};
    }
    // Half the advertised rate, and a burst of the same size but never more
    // than half the advertised burst: 50/s with a burst of 50 at 100/200, so
    // the 150 frames between the two bursts absorb frames the network bunches.
    return {rate / 2.0, std::max(1.0, std::floor(std::min(rate, burst) / 2.0))};
}

RemoteEngineScheduler::RemoteEngineScheduler(std::function<qint64()> clockMs)
    : m_clock(std::move(clockMs))
{
    m_lastRefillMs = m_clock();
    m_tokens = m_limits.requestBurst;
}

void RemoteEngineScheduler::setLimits(Limits limits)
{
    refill();
    m_limits = limits;
    m_tokens = std::min(m_tokens, m_limits.requestBurst);
}

void RemoteEngineScheduler::refill()
{
    const qint64 now = m_clock();
    const qint64 elapsed = std::max<qint64>(0, now - m_lastRefillMs);
    m_lastRefillMs = std::max(now, m_lastRefillMs);
    m_tokens = std::min(m_limits.requestBurst,
                        m_tokens + double(elapsed) * m_limits.requestsPerSecond / 1000.0);
}

double RemoteEngineScheduler::tokens()
{
    refill();
    return m_tokens;
}

void RemoteEngineScheduler::enqueuePlain(const QString& method, const QJsonObject& params)
{
    m_plain.append(Frame{method, params, {}, false});
}

void RemoteEngineScheduler::submitIntent(const QString& resourceKey, const QString& method,
                                         const QJsonObject& params, bool retried)
{
    Pending& pending = m_intents[intentKey(resourceKey, method)];
    pending.order = ++m_order;
    pending.frame = Frame{method, params, resourceKey, retried};
}

bool RemoteEngineScheduler::hasQueuedIntent(const QString& resourceKey, const QString& method) const
{
    return m_intents.count(intentKey(resourceKey, method)) != 0;
}

bool RemoteEngineScheduler::intentOutstanding(const QString& resourceKey, const QString& method) const
{
    return hasQueuedIntent(resourceKey, method)
        || m_inFlightMethods.contains(intentKey(resourceKey, method));
}

bool RemoteEngineScheduler::eligibleIntentWaiting() const
{
    return std::any_of(m_intents.begin(), m_intents.end(), [this](const auto& entry) {
        return !m_inFlight.contains(entry.second.frame.resourceKey);
    });
}

std::optional<RemoteEngineScheduler::Frame> RemoteEngineScheduler::takeNext()
{
    if (m_plain.isEmpty() && !eligibleIntentWaiting()) { return std::nullopt; }
    refill();
    if (m_tokens < 1.0) { return std::nullopt; }
    if (!m_plain.isEmpty()) {
        m_tokens -= 1.0;
        return m_plain.takeFirst();
    }
    auto oldest = m_intents.end();
    for (auto it = m_intents.begin(); it != m_intents.end(); ++it) {
        if (m_inFlight.contains(it->second.frame.resourceKey)) { continue; }
        if (oldest == m_intents.end() || it->second.order < oldest->second.order) { oldest = it; }
    }
    Frame frame = std::move(oldest->second.frame);
    m_intents.erase(oldest);
    m_inFlight.insert(frame.resourceKey);
    m_inFlightMethods.insert(intentKey(frame.resourceKey, frame.method));
    m_tokens -= 1.0;
    return frame;
}

std::optional<qint64> RemoteEngineScheduler::msUntilReady()
{
    if (m_plain.isEmpty() && !eligibleIntentWaiting()) { return std::nullopt; }
    refill();
    if (m_tokens >= 1.0) { return 0; }
    return qint64(std::ceil((1.0 - m_tokens) * 1000.0 / m_limits.requestsPerSecond));
}

void RemoteEngineScheduler::completed(const QString& resourceKey)
{
    m_inFlight.remove(resourceKey);
    const QString prefix = resourceKey + QLatin1Char('\n');
    for (auto it = m_inFlightMethods.begin(); it != m_inFlightMethods.end();) {
        it = it->startsWith(prefix) ? m_inFlightMethods.erase(it) : std::next(it);
    }
}

void RemoteEngineScheduler::clear()
{
    m_plain.clear();
    m_intents.clear();
    m_inFlight.clear();
    m_inFlightMethods.clear();
}

int RemoteEngineScheduler::queuedCount() const
{
    return int(m_plain.size() + qsizetype(m_intents.size()));
}

} // namespace AetherSDR::aetherd
