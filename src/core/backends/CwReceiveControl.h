#pragma once

#include <QSet>

namespace AetherSDR {

enum class CwAutoTuneAction { Once, Start, Stop };

inline bool validCwAutoTuneAction(CwAutoTuneAction action)
{
    return action == CwAutoTuneAction::Once || action == CwAutoTuneAction::Start
        || action == CwAutoTuneAction::Stop;
}

// Desktop receive operations. This record grants neither keying nor daemon access.
struct CwReceiveControl {
    QSet<CwAutoTuneAction> actions;
    bool supports(CwAutoTuneAction action) const { return actions.contains(action); }
};

} // namespace AetherSDR
