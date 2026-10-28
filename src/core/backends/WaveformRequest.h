#pragma once

#include "FileTransferRequest.h"

namespace AetherSDR {

enum class WaveformAction { Uninstall, RemoveContainer, Restart };
struct WaveformControl {
    QSet<WaveformAction> actions;
    bool supports(WaveformAction action) const { return actions.contains(action); }
};
struct WaveformRequest {
    WaveformAction action{WaveformAction::Restart};
    QString name;
    bool valid() const
    {
        return action >= WaveformAction::Uninstall && action <= WaveformAction::Restart
            && transferTokenValid(name, 64);
    }
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::WaveformRequest)
