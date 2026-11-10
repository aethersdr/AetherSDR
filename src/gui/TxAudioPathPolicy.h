#pragma once

#include "core/backends/RadioCapabilities.h"
#include <QStringView>

namespace AetherSDR {

enum class TxAudioPathBlock { None, PcAudio, MicInput, NoInputDevice };

// UI availability only: this never changes the radio's selected audio route.
// NoInputDevice: seam TX audio is on but the computer has no capture device,
// so neither AetherTX nor PC-mic voice has a source (#7078).
[[nodiscard]] inline TxAudioPathBlock classifyTxAudioPath(
    bool connected, const RadioCapabilities& caps, bool pcAudioEnabled,
    QStringView micSelection, bool txInputDeviceMissing = false)
{
    if (!connected || !caps.canTransmit) {
        return TxAudioPathBlock::None;
    }
    const bool noSeamInput =
        caps.takesTxAudioOverSeam && pcAudioEnabled && txInputDeviceMissing;
    if (caps.hostModulates) {
        return noSeamInput ? TxAudioPathBlock::NoInputDevice : TxAudioPathBlock::None;
    }
    if (caps.takesTxAudioOverSeam && !pcAudioEnabled) {
        return TxAudioPathBlock::PcAudio;
    }
    if (caps.hasSelectableMicInputs && micSelection != u"PC") {
        return TxAudioPathBlock::MicInput;
    }
    if (noSeamInput) {
        return TxAudioPathBlock::NoInputDevice;
    }
    return TxAudioPathBlock::None;
}

} // namespace AetherSDR
