#pragma once

#include "FileTransferRequest.h"
#include "WaveformEnvRequest.h"

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
            && waveformNameTokenValid(name);
    }
};
// The radio's verb for each action (`waveform <verb> <name>`).
inline QString waveformActionVerb(WaveformAction action)
{
    switch (action) {
    case WaveformAction::Uninstall: return QStringLiteral("uninstall");
    case WaveformAction::RemoveContainer: return QStringLiteral("remove_container");
    case WaveformAction::Restart: break;
    }
    return QStringLiteral("restart");
}

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::WaveformRequest)
