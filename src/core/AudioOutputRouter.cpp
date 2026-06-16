#include "AudioOutputRouter.h"

namespace AetherSDR {

AudioOutputRouter::AudioOutputRouter(QObject* parent)
    : QObject(parent)
{
}

void AudioOutputRouter::addFollower(std::function<void(const QAudioDevice&)> apply)
{
    if (!apply)
        return;
    // Seed the new follower with the current selection so it starts on the right
    // device without waiting for the next change.
    apply(m_device);
    m_followers.push_back(std::move(apply));
}

void AudioOutputRouter::setCurrentDevice(const QAudioDevice& dev)
{
    m_device = dev;
    // Fan out over a snapshot: a follower's setOutputDevice() could register
    // another follower (or otherwise mutate m_followers) during the callback,
    // which would invalidate a live iterator / reallocate the vector mid-loop.
    // A follower added during the fan-out is already seeded by addFollower(), so
    // skipping it here is correct, not a miss. The list is tiny (registered once
    // at startup) so the copy is negligible.
    const auto followers = m_followers;
    for (const auto& apply : followers) {
        if (apply)
            apply(m_device);
    }
}

} // namespace AetherSDR
