#include "VoiceKeyer.h"

namespace AetherSDR {

VoiceKeyer::VoiceKeyer(QObject* parent) : QObject(parent) {}

VoiceKeyer::~VoiceKeyer() = default;

// Idle admits a new operation; Unknown (no status reported yet) fails open so
// a keyer that has not spoken cannot deadlock the panel. Anything else means
// something is already running.
void VoiceKeyer::playbackStartFrom(int id, const TxCoordinator::Request& origin)
{
    if (!origin.valid()) {
        emit commandFailed(QStringLiteral("playback_start"), id, 0,
                           QStringLiteral("The control that asked for playback is gone."));
        return;
    }
    playbackStart(id);
}

bool VoiceKeyer::canStartOperation() const
{
    const Status s = status();
    return s == Idle || s == Unknown;
}

QString VoiceKeyer::defaultSlotName(int id) const
{
    return QStringLiteral("Recording %1").arg(id);
}

} // namespace AetherSDR
