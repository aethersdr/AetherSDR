#include "TxTimeoutGuard.h"

#include <algorithm>

namespace AetherSDR {

TxTimeoutSettings TxTimeoutSettings::defaultsFor(int radioTimeoutMs)
{
    TxTimeoutSettings settings;
    if (radioTimeoutMs > 0) {
        const int radioS = (radioTimeoutMs + 999) / 1000;
        settings.timeoutS = std::max(settings.timeoutS, radioS);
    }
    return settings.clamped();
}

TxTimeoutSettings TxTimeoutSettings::fromJson(const QJsonObject& doc,
                                              const TxTimeoutSettings& fallback)
{
    TxTimeoutSettings settings = fallback;
    if (doc.value(QStringLiteral("enabled")).isBool()) {
        settings.enabled = doc.value(QStringLiteral("enabled")).toBool();
    }
    if (doc.value(QStringLiteral("timeoutS")).isDouble()) {
        settings.timeoutS = doc.value(QStringLiteral("timeoutS")).toInt(settings.timeoutS);
    }
    if (doc.value(QStringLiteral("warningS")).isDouble()) {
        settings.warningS = doc.value(QStringLiteral("warningS")).toInt(settings.warningS);
    }
    return settings.clamped();
}

QJsonObject TxTimeoutSettings::toJson() const
{
    return QJsonObject{{QStringLiteral("enabled"), enabled},
                       {QStringLiteral("timeoutS"), timeoutS},
                       {QStringLiteral("warningS"), warningS}};
}

TxTimeoutSettings TxTimeoutSettings::clamped() const
{
    TxTimeoutSettings settings = *this;
    settings.timeoutS = std::clamp(settings.timeoutS, kMinimumTimeoutS, kMaximumTimeoutS);
    settings.warningS = std::clamp(settings.warningS, 0, kMaximumWarningS);
    return settings;
}

TxTimeoutGuard::TxTimeoutGuard(Clock clock)
    : m_clock(std::move(clock))
{
}

void TxTimeoutGuard::setSettings(const TxTimeoutSettings& settings)
{
    m_settings = settings.clamped();
    if (!m_settings.enabled) {
        m_armed = false;
    }
}

void TxTimeoutGuard::setTransmitting(bool on, const QString& source)
{
    if (on == m_transmitting) {
        return;
    }
    m_transmitting = on;
    if (!on) {
        m_armed = false;
        refreshLatch();
        return;
    }
    if (!m_settings.enabled || !m_clock) {
        return;
    }
    m_armed = true;
    m_warned = false;
    m_startedMs = m_clock();
    m_source = source;
}

qint64 TxTimeoutGuard::remainingMs() const
{
    if (!m_armed || !m_clock) {
        return 0;
    }
    return std::max<qint64>(0, m_startedMs + qint64{m_settings.timeoutS} * 1000 - m_clock());
}

TxTimeoutGuard::Event TxTimeoutGuard::poll()
{
    refreshLatch();
    if (!m_armed || !m_clock) {
        return Event::None;
    }
    const qint64 elapsed = m_clock() - m_startedMs;
    const qint64 limit = qint64{m_settings.timeoutS} * 1000;
    if (elapsed >= limit) {
        m_armed = false;
        m_latched = true;
        m_lastExpiredAfterS = m_settings.timeoutS;
        return Event::Expired;
    }
    const int warningS = m_settings.warningS;
    if (!m_warned && warningS > 0 && warningS < m_settings.timeoutS
        && elapsed >= limit - qint64{warningS} * 1000) {
        m_warned = true;
        return Event::Warning;
    }
    return Event::None;
}

bool TxTimeoutGuard::canHold(quintptr key)
{
    if (!key || m_holders.contains(key)
        || m_holders.size() < static_cast<std::size_t>(kMaximumHolders)) {
        return true;
    }
    // Evict only the dead: a live holder is never dropped to make room.
    std::erase_if(m_holders, [](const auto& entry) { return !entry.second || !entry.second(); });
    return m_holders.size() < static_cast<std::size_t>(kMaximumHolders);
}

bool TxTimeoutGuard::noteHold(quintptr key, std::function<bool()> alive)
{
    if (!key) {
        return true;
    }
    if (!canHold(key)) {
        return false;
    }
    m_holders[key] = std::move(alive);
    return true;
}

void TxTimeoutGuard::noteRelease(quintptr key)
{
    if (m_holders.erase(key) > 0) {
        refreshLatch();
    }
}

void TxTimeoutGuard::forgetHolders()
{
    if (!m_latched) {
        m_holders.clear();
    }
}

bool TxTimeoutGuard::refreshLatch()
{
    std::erase_if(m_holders, [](const auto& entry) { return !entry.second || !entry.second(); });
    if (m_latched && m_holders.empty() && !m_transmitting) {
        m_latched = false;
        return true;
    }
    return false;
}

void TxTimeoutGuard::reset()
{
    m_transmitting = false;
    m_armed = false;
    m_warned = false;
    m_latched = false;
    m_holders.clear();
}

} // namespace AetherSDR
