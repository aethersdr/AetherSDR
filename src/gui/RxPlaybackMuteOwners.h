#pragma once

#include <QSet>
#include <QString>

namespace AetherSDR {

// Who is currently holding live RX audio off the shared sink. Three producers
// ask independently — the QSO recorder, the PUDU monitor and a voice keyer
// preview — and they can overlap, so the mute is held while any of them wants
// it and lifted only when the last one is done.
//
// set() answers what changed, not what is wanted: the caller's work on a mute
// edge is to drop a Qt connection and on an unmute edge to restore it, and Qt
// permits duplicate connections, so acting on a repeated request would stack
// another copy of the feed.
class RxPlaybackMuteOwners {
public:
    enum class Edge { None, Mute, Unmute };

    Edge set(const QString& owner, bool mute)
    {
        const bool was = muted();
        if (mute)
            m_owners.insert(owner);
        else
            m_owners.remove(owner);
        if (muted() == was)
            return Edge::None;
        return muted() ? Edge::Mute : Edge::Unmute;
    }

    bool muted() const { return !m_owners.isEmpty(); }

private:
    QSet<QString> m_owners;
};

}  // namespace AetherSDR
