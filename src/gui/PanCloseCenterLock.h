#pragma once

#include <QHash>
#include <QString>

// What an operator's pan close does to that pan's Center Lock. The saved
// intent is keyed by the pan's slot index, read before the close: a native
// close (HL2, ANAN) destroys the widget that carries the index.
namespace AetherSDR::PanCloseCenterLock {

// radio key -> pan slot index -> locked slice letter
using SavedIntents = QHash<QString, QHash<int, QString>>;

struct Slot {
    QString radioKey;
    int panIndex{-1};
    bool sessionOnly{false};   // a TCI client's lock never owned the saved intent
};

// Removes the saved intent for one slot; true when the store changed.
inline bool forgetSaved(SavedIntents& saved, const Slot& slot)
{
    if (slot.radioKey.isEmpty() || slot.panIndex < 0) {
        return false;
    }
    const auto radioIt = saved.find(slot.radioKey);
    if (radioIt == saved.end() || !radioIt->contains(slot.panIndex)) {
        return false;
    }
    radioIt->remove(slot.panIndex);
    if (radioIt->isEmpty()) {
        saved.erase(radioIt);
    }
    return true;
}

// Runs close(); only once it is accepted, forgets the live lock (forgetLive)
// and, unless session-only, the saved intent. A refused close keeps both
// (#7359 ruling). Returns close()'s answer; savedChanged tells the caller to
// persist the store.
template <class Close, class ForgetLive>
bool closeThenForget(const Slot& slot, SavedIntents& saved, Close&& close,
                     ForgetLive&& forgetLive, bool& savedChanged)
{
    savedChanged = false;
    if (!close()) {
        return false;
    }
    forgetLive();
    savedChanged = !slot.sessionOnly && forgetSaved(saved, slot);
    return true;
}

} // namespace AetherSDR::PanCloseCenterLock
