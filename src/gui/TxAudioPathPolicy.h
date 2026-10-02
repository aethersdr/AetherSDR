#pragma once

#include "core/backends/RadioCapabilities.h"
#include <QStringView>

namespace AetherSDR {

enum class TxAudioPathBlock { None, PcAudio, MicInput };

// UI availability only: this never changes the radio's selected audio route.
[[nodiscard]] inline TxAudioPathBlock classifyTxAudioPath(
    bool connected, const RadioCapabilities& caps, bool pcAudioEnabled,
    QStringView micSelection)
{
    if (!connected || !caps.canTransmit || caps.hostModulates) {
        return TxAudioPathBlock::None;
    }
    if (caps.takesTxAudioOverSeam && !pcAudioEnabled) {
        return TxAudioPathBlock::PcAudio;
    }
    if (caps.hasSelectableMicInputs && micSelection != u"PC") {
        return TxAudioPathBlock::MicInput;
    }
    return TxAudioPathBlock::None;
}

} // namespace AetherSDR
