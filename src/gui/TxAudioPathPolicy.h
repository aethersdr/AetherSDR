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

// Which NoInputDevice text fits: a host-modulated radio sends no voice RF at
// all; any other seam radio may still be fed by its own microphone.
enum class NoInputDeviceWording { NoVoiceRf, NoComputerVoice };

[[nodiscard]] inline NoInputDeviceWording noInputDeviceWording(const RadioCapabilities& caps)
{
    return caps.hostModulates ? NoInputDeviceWording::NoVoiceRf
                              : NoInputDeviceWording::NoComputerVoice;
}

// Whether an audio device change should retry TX capture. A PC mic selection
// (Flex, HL2) or the seam route with PC Audio on, as when the capabilities
// arrive: a networked Icom keeps micSelection "MIC" (#7078).
[[nodiscard]] inline bool txCaptureRetryWanted(
    bool connected, const RadioCapabilities& caps, bool pcAudioEnabled,
    QStringView micSelection)
{
    if (!connected) {
        return false;
    }
    if (micSelection == u"PC") {
        return true;
    }
    return caps.takesTxAudioOverSeam && caps.canTransmit
        && (pcAudioEnabled || caps.hostModulates);
}

// A radio this computer modulates and may key: the HL2 today. The two #7078
// follow-ups below apply to it only; Flex and Icom keep their behaviour.
[[nodiscard]] inline bool hostModulatedTransmit(const RadioCapabilities& caps)
{
    return caps.hostModulates && caps.canTransmit;
}

// The chain's MIC endpoint: the computer mic feeds the client DSP only with
// PC selected and radio DAX TX off. On a host-modulated radio it also needs a
// capture device (#7078).
[[nodiscard]] inline bool pcMicChainReady(const RadioCapabilities& caps,
                                          QStringView micSelection, bool daxOn,
                                          bool txInputDeviceMissing)
{
    const bool deviceMissing = hostModulatedTransmit(caps) && txInputDeviceMissing;
    return micSelection == u"PC" && !daxOn && !deviceMissing;
}

// A key only the computer mic can modulate. operatorKeyed is
// RadioModel::isOperatorTransmitting(), which already leaves out TUNE/ATU,
// CW, DAX and TCI; WSPR and the data modes have sources of their own, except
// RADE, which encodes the computer mic on a DIGU/DIGL slice.
[[nodiscard]] inline bool isMicVoiceKey(bool operatorKeyed, bool sourceIsWspr,
                                        QStringView txMode, bool radeActive = false)
{
    if (!operatorKeyed || sourceIsWspr) {
        return false;
    }
    if (radeActive) {
        return true;
    }
    const QStringView mode = txMode.trimmed();
    return !mode.startsWith(u"CW", Qt::CaseInsensitive)
        && mode.compare(u"DIGU", Qt::CaseInsensitive) != 0
        && mode.compare(u"DIGL", Qt::CaseInsensitive) != 0;
}

// Once per stretch of NoInputDevice on a host-modulated radio: a voice key
// raises the notice the first time only, and it re-arms when the route leaves
// that state (an input device is found, PC Audio goes off, the radio
// disconnects). Other radios never raise it. It never refuses a key.
class NoInputVoiceKeyNotice {
public:
    void observe(TxAudioPathBlock block)
    {
        if (block != TxAudioPathBlock::NoInputDevice) {
            m_shown = false;
        }
    }
    [[nodiscard]] bool takeVoiceKey(const RadioCapabilities& caps, TxAudioPathBlock block)
    {
        observe(block);
        if (!hostModulatedTransmit(caps) || block != TxAudioPathBlock::NoInputDevice
            || m_shown) {
            return false;
        }
        m_shown = true;
        return true;
    }

private:
    bool m_shown{false};
};

} // namespace AetherSDR
