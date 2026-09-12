#include "core/aprs/AprsFillInDigipeater.h"

namespace AetherSDR {

using ax25::Address;
using ax25::Frame;
using ax25::FrameType;

AprsFillInDigipeater::AprsFillInDigipeater(QObject* parent)
    : QObject(parent)
{
    if (auto a = Address::parse(QStringLiteral("WIDE1-1")))
        m_alias = *a;
}

void AprsFillInDigipeater::setDupeWindowSecs(int secs)
{
    m_dupeSecs = qBound(5, secs, 300);
}

QString AprsFillInDigipeater::tnc2(const Frame& frame)
{
    QString line = frame.src.toString() + QLatin1Char('>') + frame.dest.toString();
    for (const Address& hop : frame.via) {
        line += QLatin1Char(',');
        line += hop.toString();
        if (hop.hasBeenRepeated)
            line += QLatin1Char('*');
    }
    line += QLatin1Char(':');
    line += QString::fromLatin1(frame.info);
    return line;
}

bool AprsFillInDigipeater::hopMatches(const Address& hop) const
{
    if (hop == m_alias)
        return true;
    if (m_alsoMyCall && m_my.isValid() && hop == m_my)
        return true;
    if (m_alsoRelay) {
        if (hop.call.compare(QLatin1String("RELAY"), Qt::CaseInsensitive) == 0
            && hop.ssid == 0)
            return true;
    }
    return false;
}

void AprsFillInDigipeater::pruneDupes(qint64 nowMs)
{
    const qint64 cutoff = nowMs - qint64(m_dupeSecs) * 1000;
    for (auto it = m_dupes.begin(); it != m_dupes.end(); ) {
        if (it.value() < cutoff)
            it = m_dupes.erase(it);
        else
            ++it;
    }
}

bool AprsFillInDigipeater::isDuplicate(const QString& key, qint64 nowMs)
{
    pruneDupes(nowMs);
    const auto it = m_dupes.constFind(key);
    return it != m_dupes.cend() && (nowMs - it.value()) < qint64(m_dupeSecs) * 1000;
}

void AprsFillInDigipeater::remember(const QString& key, qint64 nowMs)
{
    m_dupes.insert(key, nowMs);
}

AprsFillInDigipeater::Decision
AprsFillInDigipeater::consider(const Frame& frame, bool recordStats)
{
    Decision d;
    const auto tally = [&](Drop drop) {
        d.drop = drop;
        if (!recordStats)
            return;
        ++m_stats.heard;
        switch (drop) {
        case Drop::Repeated:     ++m_stats.repeated; break;
        case Drop::Duplicate:    ++m_stats.droppedDupe; break;
        case Drop::Own:          ++m_stats.droppedOwn; break;
        case Drop::NoAliasMatch:
        case Drop::NoUnusedHop:  ++m_stats.droppedNoMatch; break;
        default:                 ++m_stats.droppedOther; break;
        }
    };

    if (frame.type != FrameType::UI || frame.info.isEmpty()) {
        d.reason = QStringLiteral("not an APRS UI frame");
        tally(Drop::NotUi);
        return d;
    }
    if (m_my.isValid() && frame.src == m_my) {
        d.reason = QStringLiteral("source is MYCALL");
        tally(Drop::Own);
        return d;
    }
    if (m_my.isValid()) {
        for (const Address& hop : frame.via) {
            if (hop == m_my && hop.hasBeenRepeated) {
                d.reason = QStringLiteral("already digipeated by us");
                tally(Drop::AlreadyHeard);
                return d;
            }
        }
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const QString key = frame.src.toString() + QLatin1Char('|')
        + frame.dest.toString() + QLatin1Char('|')
        + QString::fromLatin1(frame.info);
    if (isDuplicate(key, nowMs)) {
        d.reason = QStringLiteral("duplicate inside dupe window");
        tally(Drop::Duplicate);
        return d;
    }

    int unused = -1;
    for (int i = 0; i < frame.via.size(); ++i) {
        if (!frame.via.at(i).hasBeenRepeated) {
            unused = i;
            break;
        }
    }
    if (unused < 0) {
        d.reason = QStringLiteral("no unused via hop");
        tally(Drop::NoUnusedHop);
        remember(key, nowMs);
        return d;
    }

    if (!hopMatches(frame.via.at(unused))) {
        d.reason = QStringLiteral("first unused hop is %1")
                       .arg(frame.via.at(unused).toString());
        tally(Drop::NoAliasMatch);
        remember(key, nowMs);
        return d;
    }

    if (!m_my.isValid()) {
        d.reason = QStringLiteral("MYCALL not set");
        tally(Drop::NoAliasMatch);
        return d;
    }

    Frame out = frame;
    Address tagged = m_my;
    tagged.hasBeenRepeated = true;
    out.via[unused] = tagged;
    d.outgoing = out;
    d.reason = QStringLiteral("WIDE1-1 fill-in as %1").arg(m_my.toString());
    tally(Drop::Repeated);
    remember(key, nowMs);
    return d;
}

} // namespace AetherSDR
