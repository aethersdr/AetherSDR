#pragma once

#include "MainWindowHelpers.h"
#include "TwoToneTuneGate.h"
#include "models/TxController.h"

#include <functional>

namespace AetherSDR {

// Shared by MIDI and MIDI-mapped native controls. A recognized TX action is
// consumed even after its captured input has expired: it must never fall
// through to a setter that captures the native operator's authority.
//
// tx.atuBypass and cwx.macro1..12 are the ControlActionRegistry's keying
// entries: a press bypasses the ATU or fires CWX macro F1..F12 (index 0..11,
// through cwxMacro) on the device's own captured input; a release is consumed.
inline bool dispatchMidiTxInput(const QString& id, float value, RadioModel& radio,
    const std::shared_ptr<TxController>& controller,
    const std::function<void(const QString&, bool, const std::shared_ptr<TxController>&)>& cwHandler,
    const std::function<void(const QString&)>& refused = {},
    const std::function<void(int, const TxController::Input&)>& cwxMacro = {})
{
    const bool atuBypass = id == QLatin1String("tx.atuBypass");
    int macro = -1;
    if (id.startsWith(QLatin1String("cwx.macro"))) {
        bool ok = false;
        const int n = id.mid(9).toInt(&ok);   // "cwx.macro" is 9 characters
        if (!ok || n < 1 || n > 12) { return false; }
        macro = n - 1;
    }
    if (atuBypass || macro >= 0) {
        if (value <= 0.5f || !controller || !controller->valid()
            || !controller->belongsTo(&radio)) {
            return true;
        }
        if (atuBypass) {
            (void)controller->capture(TxController::Activity::Atu).bypassAtu();
        } else if (cwxMacro) {
            cwxMacro(macro, controller->capture(TxController::Activity::Cwx));
        }
        return true;
    }
    const bool cw = id == QLatin1String(kCwStraightKeyActionId)
        || id == QLatin1String(kCwLeftPaddleActionId)
        || id == QLatin1String(kCwRightPaddleActionId);
    const bool mox = id == QLatin1String("tx.mox") || id == QLatin1String("global.txButton")
        || id == QLatin1String("cw.ptt");
    const bool twoTone = id == QLatin1String("global.twoToneTune");
    const bool tune = id == QLatin1String("tx.tune") || twoTone;
    const bool atu = id == QLatin1String("tx.atuStart");
    if (!cw && !mox && !tune && !atu) { return false; }
    if (!controller || !controller->valid() || !controller->belongsTo(&radio)) { return true; }
    if (cw) {
        if (cwHandler) { cwHandler(id, value > 0.5f, controller); }
        return true;
    }

    const TxController::Activity activity = atu ? TxController::Activity::Atu
        : tune ? TxController::Activity::Tune : TxController::Activity::Mox;
    const bool on = atu || (twoTone ? !radio.transmitModel().isTuning()
        : value == -1.0f
            ? !(tune ? radio.transmitModel().isTuning() : radio.transmitModel().isTransmitting())
            : value > 0.5f);
    // A start on a radio with no two-tone generator is refused here, before the
    // input is captured or any keying call is made; the action is consumed.
    if (twoTone && on && !twoToneTuneMayStart(radio, refused)) { return true; }
    const TxController::Input input = on ? controller->capture(activity) : controller->current(activity);
    if (on) {
        (void)input.start(twoTone);
    } else {
        const bool ownedTune = twoTone && input.active();
        const QPointer<RadioModel> guard(&radio);
        input.stop();
        // Mirror the two-tone shortcut's one-shot mode, but only after our
        // own tune stopped. A foreign tune or synchronous replacement keeps
        // its mode, and a callback cannot restore into a later radio session.
        if (ownedTune && guard && controller->valid() && !guard->transmitModel().isTuning()) {
            guard->transmitModel().setTuneMode(QStringLiteral("single_tone"));
        }
    }
    return true;
}

} // namespace AetherSDR
