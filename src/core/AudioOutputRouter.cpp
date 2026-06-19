#include "AudioOutputRouter.h"

namespace AetherSDR {

AudioOutputRouter::AudioOutputRouter(QObject* parent)
    : QObject(parent)
{
}

void AudioOutputRouter::addFollower(std::function<void(const QAudioDevice&)> apply)
{
    // Raw callback follower: no liveness info, so it's always considered alive
    // (the caller owns its lifetime).
    registerFollower(std::move(apply), nullptr);
}

void AudioOutputRouter::registerFollower(std::function<void(const QAudioDevice&)> apply,
                                         std::function<bool()> alive)
{
    if (!apply)
        return;
    // Seed the new follower with the current selection so it starts on the right
    // device without waiting for the next change.
    apply(m_device);
    m_followers.push_back({std::move(apply), std::move(alive)});
}

void AudioOutputRouter::setCurrentDevice(const QAudioDevice& dev)
{
    m_device = dev;
    // Fan out over a snapshot: a follower's setOutputDevice() could register
    // another follower (or otherwise mutate m_followers) during the callback,
    // which would invalidate a live iterator / reallocate the vector mid-loop.
    // A follower added during the fan-out is already seeded by registerFollower(),
    // so skipping it here is correct, not a miss. The list is tiny (registered
    // once at startup) so the copy is negligible.
    const auto followers = m_followers;
    for (const auto& f : followers) {
        if (f.apply)
            f.apply(m_device);   // guarded followers self-skip when their QPointer is null
    }
    // Prune followers whose QPointer guard has gone null so dead entries don't
    // accumulate over the router's lifetime (#3660). Done after the fan-out on
    // the live vector; a follower added re-entrantly during the loop is alive
    // and therefore kept. Raw std::function followers have no `alive` predicate
    // and are never pruned (the caller owns their lifetime).
    std::erase_if(m_followers, [](const Follower& f) {
        return f.alive && !f.alive();
    });
}

} // namespace AetherSDR
