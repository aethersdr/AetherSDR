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
    OperatorChoice,  // picked in Radio Setup or in the hot-plug prompt
    Fallback,        // the app re-pointing itself: device gone, or back again
};

constexpr bool persistsAudioDeviceSelection(AudioDeviceSelection selection)
{
    return selection == AudioDeviceSelection::OperatorChoice;
}

// One direction's view of a device-list change.
struct AudioDeviceListChange {
    // Every enumerable id, including the ones the hot-plug monitor ignores as
    // churn; addedIds leaves those out.
    QList<QByteArray> presentIds;
    QList<QByteArray> addedIds;    // enumerable now, not at the previous check
    // The enumerable ids, churn left out: the next check's knownArrivalIds.
    // Set by audioDeviceListChange(); decideAudioDeviceChange() does not read it.
    QList<QByteArray> arrivalIds;
    bool anyRemoved{false};        // some id from the previous check is gone
    bool defaultChanged{false};    // the system default id moved
    QByteArray liveId;             // engine's device; empty = system default
    QByteArray preferredId;        // saved id; empty = none saved
};

// Which enumerable ids the hot-plug monitor ignores as audio-server churn.
enum class AudioDeviceChurnRule {
    None,   // macOS, Windows: every id counts
    Linux,  // PipeWire / PulseAudio virtual devices are left out
};

#ifdef Q_OS_LINUX
inline constexpr AudioDeviceChurnRule kHostAudioDeviceChurnRule = AudioDeviceChurnRule::Linux;
#else
inline constexpr AudioDeviceChurnRule kHostAudioDeviceChurnRule = AudioDeviceChurnRule::None;
#endif

// Monitor sources, per-app loopbacks, the auto-null sink and the echo-cancel /
// combine virtuals come and go constantly and are never a new mic or speaker
// to offer (#2864). Compiled on every platform so tests can exercise the Linux
// rule on any host.
inline bool isLinuxTransientAudioDeviceId(const QByteArray& id)
{
    return id.contains(".monitor")
        || id.startsWith("pulse_input_loopback")
        || id.contains("auto_null")
        || id.contains("echo-cancel")
        || id.contains("combined");
}

inline bool isTransientAudioDeviceId(const QByteArray& id,
                                     AudioDeviceChurnRule rule = kHostAudioDeviceChurnRule)
{
    return rule == AudioDeviceChurnRule::Linux && isLinuxTransientAudioDeviceId(id);
}

// The ids that count as arrivals and removals: the enumerable ids, churn left
// out. Never the answer to "is this device present".
inline QList<QByteArray> arrivalAudioDeviceIds(
    const QList<QByteArray>& enumeratedIds,
    AudioDeviceChurnRule rule = kHostAudioDeviceChurnRule)
{
    QList<QByteArray> ids;
    ids.reserve(enumeratedIds.size());
    for (const QByteArray& id : enumeratedIds) {
        if (!isTransientAudioDeviceId(id, rule)) {
            ids.append(id);
        }
    }
    return ids;
}

// One direction's change, from every enumerable id now and the arrival list of
// the previous check. Presence reads the enumerable ids; arrivals and removals
// read the arrival lists.
struct AudioDeviceListSnapshot {
    QList<QByteArray> enumeratedIds;    // every enumerable id now
    QList<QByteArray> knownArrivalIds;  // the arrival list of the previous check
    bool defaultChanged{false};
    QByteArray liveId;
    QByteArray preferredId;
};

inline AudioDeviceListChange audioDeviceListChange(
    const AudioDeviceListSnapshot& now,
    AudioDeviceChurnRule rule = kHostAudioDeviceChurnRule)
{
    AudioDeviceListChange change;
    change.presentIds = now.enumeratedIds;
    change.arrivalIds = arrivalAudioDeviceIds(now.enumeratedIds, rule);
    for (const QByteArray& id : change.arrivalIds) {
        if (!now.knownArrivalIds.contains(id)) {
            change.addedIds.append(id);
        }
    }
    for (const QByteArray& id : now.knownArrivalIds) {
        if (!change.arrivalIds.contains(id)) {
            change.anyRemoved = true;
        }
    }
    change.defaultChanged = now.defaultChanged;
    change.liveId = now.liveId;
    change.preferredId = now.preferredId;
    return change;
}

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

// How one direction's selection in the accepted hot-plug prompt is applied.
// The prompt lists present devices only, so with the saved device absent
// "System default" is all it can show there: that follows the default for
// this run and keeps the saved id. Every other selection is the operator's.
inline AudioDeviceSelection acceptedAudioDeviceSelection(const QByteArray& selectedId,
                                                         const QByteArray& preferredId,
                                                         const QList<QByteArray>& presentIds)
{
    const bool preferredAbsent =
        !preferredId.isEmpty() && !presentIds.contains(preferredId);
    return selectedId.isEmpty() && preferredAbsent ? AudioDeviceSelection::Fallback
                                                   : AudioDeviceSelection::OperatorChoice;
}

} // namespace AetherSDR
