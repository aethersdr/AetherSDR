#include "core/NetScheduler.h"

namespace AetherSDR {

namespace {
// Cap a single sleep at ~24h so a far-future reminder still wakes us daily to
// recompute against the real clock (handles suspend, time changes, DST).
constexpr qint64 kMaxSleepMs = 24LL * 60 * 60 * 1000;
// Arm a hair past the reminder instant so the planner's strictly-after query
// reliably excludes it on the next rearm.
constexpr int kArmGuardMs = 200;

QString firedKey(const NetEntry& entry, const QDateTime& occurrenceUtc)
{
    return entry.id + '|' + QString::number(occurrenceUtc.toMSecsSinceEpoch());
}
} // namespace

NetScheduler::NetScheduler(QObject* parent)
    : QObject(parent)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &NetScheduler::onTimeout);
}

QDateTime NetScheduler::nowUtc() const
{
    return QDateTime::currentDateTimeUtc();
}

void NetScheduler::setEntries(const QList<NetEntry>& entries)
{
    m_entries = entries;
    rearm();
}

void NetScheduler::rearm()
{
    m_timer.stop();
    const QDateTime now = nowUtc();

    // Prune fired keys for occurrences now safely in the past.
    for (auto it = m_firedKeys.begin(); it != m_firedKeys.end();) {
        const qint64 occMs = it->section('|', 1).toLongLong();
        if (occMs < now.toMSecsSinceEpoch() - 60000)
            it = m_firedKeys.erase(it);
        else
            ++it;
    }

    m_armed = nextReminderAcross(m_entries, now);
    if (!m_armed.isValid())
        return;

    const qint64 ms = now.msecsTo(m_armed.reminderUtc);
    const qint64 sleep = qBound<qint64>(0, ms, kMaxSleepMs) + kArmGuardMs;
    m_timer.start(static_cast<int>(qMin<qint64>(sleep, kMaxSleepMs + kArmGuardMs)));
}

void NetScheduler::onTimeout()
{
    const QDateTime now = nowUtc();

    if (m_armed.isValid() && now >= m_armed.reminderUtc
        && m_armed.entryIndex >= 0 && m_armed.entryIndex < m_entries.size()) {
        const NetEntry entry = m_entries.at(m_armed.entryIndex);
        const QString key = firedKey(entry, m_armed.occurrenceUtc);
        if (!m_firedKeys.contains(key)) {
            m_firedKeys.insert(key);
            Q_EMIT reminderDue(entry, m_armed.occurrenceUtc);
        }
    }

    rearm();
}

} // namespace AetherSDR
