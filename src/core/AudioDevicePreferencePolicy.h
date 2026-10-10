#pragma once

#include <QByteArray>
#include <QList>

// The saved PC audio device (AudioOutputDeviceId / AudioInputDeviceId) is the
// operator's preference; the device the engine runs on is the live selection.
// They differ while the preferred device is absent (#6040). Pure header, one
// direction per call; rows in tests/audio_device_preference_policy_test.cpp.

namespace AetherSDR {

// Why the engine's device is being set. A fallback changes the live device for
// this run and leaves the saved id alone.
enum class AudioDeviceSelection {
    OperatorChoice,  // picked in Radio Setup or accepted in the hot-plug prompt
    Fallback,        // the app re-pointing itself: device gone, or back again
};

constexpr bool persistsAudioDeviceSelection(AudioDeviceSelection selection)
{
    return selection == AudioDeviceSelection::OperatorChoice;
}

// One direction's view of a device-list change.
struct AudioDeviceListChange {
    QList<QByteArray> presentIds;  // enumerable now
    QList<QByteArray> addedIds;    // enumerable now, not at the previous check
    bool anyRemoved{false};        // some id from the previous check is gone
    bool defaultChanged{false};    // the system default id moved
    QByteArray liveId;             // engine's device; empty = system default
    QByteArray preferredId;        // saved id; empty = none saved
};

enum class AudioDeviceAction {
    Keep,              // leave the engine's device alone
    FollowDefault,     // re-point at the system default; saved id untouched
    RestorePreferred,  // re-point at the saved device, which just arrived
};

struct AudioDeviceDecision {
    AudioDeviceAction action{AudioDeviceAction::Keep};
    // The engine's device is no longer enumerable. With a device added in the
    // same change, this is what asks the operator to choose.
    bool liveMissing{false};
};

inline AudioDeviceDecision decideAudioDeviceChange(const AudioDeviceListChange& change)
{
    AudioDeviceDecision decision;
    const bool followsDefault = change.liveId.isEmpty();
    decision.liveMissing = !followsDefault && !change.presentIds.contains(change.liveId);

    // Only a run that sits on the system default returns to the saved device:
    // a live device the operator is using is never swapped under them.
    if (followsDefault && !change.preferredId.isEmpty()
        && change.addedIds.contains(change.preferredId)) {
        decision.action = AudioDeviceAction::RestorePreferred;
    } else if (decision.liveMissing) {
        decision.action = AudioDeviceAction::FollowDefault;
    } else if (followsDefault && (change.anyRemoved || change.defaultChanged)) {
        // The default route moved under a stream that follows it: restart.
        decision.action = AudioDeviceAction::FollowDefault;
    }
    return decision;
}

// A new arrival while both live devices still work is platform churn, not a
// question for the operator (#2864).
inline bool audioDeviceChoiceRequired(bool anyDeviceAdded,
                                      const AudioDeviceDecision& input,
                                      const AudioDeviceDecision& output)
{
    return anyDeviceAdded && (input.liveMissing || output.liveMissing);
}

} // namespace AetherSDR
