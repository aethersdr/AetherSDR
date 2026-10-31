#include "VoiceKeyerController.h"

#include "core/AudioEngine.h"
#include "core/AudioOutputRouter.h"
#include "core/GeneratedAudioTransmitter.h"
#include "core/LocalWavPlayer.h"
#include "core/VoiceKeyerSettings.h"
#include "core/VoiceKeyerTxRoute.h"
#include "models/DvkModel.h"
#include "models/LocalVoiceKeyer.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"
#include "models/VoiceKeyer.h"
#include "models/VoiceKeyerRecordGate.h"

#include <utility>

namespace AetherSDR {

VoiceKeyerController::VoiceKeyerController(RadioModel& radio,
                                           AudioEngine* audio,
                                           AudioOutputRouter* outputRouter,
                                           QObject* parent)
    : QObject(parent), m_radio(radio), m_audio(audio)
{
    m_local = new LocalVoiceKeyer(VoiceKeyerSettings::recordingsDir(), this);
    m_player = new LocalWavPlayer(this);
    if (outputRouter)
        outputRouter->addFollower(m_player);

    m_local->setRecordRefusalProbe([this]() -> QString {
        return voiceKeyerRecordRefusal(m_radio.backendCapabilities().hasSelectableMicInputs,
                                       m_radio.transmitModel().micSelection());
    });
    // Recording is the operator talking into the microphone, and VOX keys on
    // the microphone, so recording with VOX on puts the radio on the air
    // (measured on a FLEX-6600). Refusing the recording does not prevent that
    // — VOX keys on the voice regardless — so the hold is what makes recording
    // safe, and the radio gets its setting back when the recording ends.
    m_voxConfirmTimer.setSingleShot(true);
    m_voxConfirmTimer.setInterval(3000);
    connect(&m_voxConfirmTimer, &QTimer::timeout, this, [this] {
        finishVoxHold(HoldOutcome::Unconfirmed);
    });
    auto& tx = m_radio.transmitModel();
    connect(&tx, &TransmitModel::voxEnableAnswered, this, &VoiceKeyerController::onVoxAnswered);
    connect(&tx, &TransmitModel::phoneStateChanged, this, &VoiceKeyerController::stopRecordingIfVoxOn);
    connect(&m_radio, &RadioModel::connectionStateChanged, this, [this](bool connected) {
        if (connected)
            return;
        // Nothing can be put back on a radio that is gone; and an answer the
        // dead link still owes will not come.
        m_restoreTicket = 0;
        if (m_holdTicket != 0)
            finishVoxHold(HoldOutcome::Unconfirmed);
    });

    m_local->setRecordGuards(
        [this]() -> bool {
            // A restore still in flight is VOX on its way back on, and an
            // operator's VOX-off the radio has not answered is only a request:
            // in both the model reads off while the radio may not be.
            const auto& tx = m_radio.transmitModel();
            const VoxHold hold = voxHoldForRecording(
                m_radio.isConnected(), m_radio.backendCapabilities().voxControl.has_value(),
                tx.voxEnable(), m_restoreTicket != 0, tx.voxStateConfirmed());
            if (hold == VoxHold::None)
                return true;   // nothing to hold, so nothing to wait for
            beginVoxHold(hold == VoxHold::HoldAndRestore);
            return false;      // recording waits for the radio's answer
        },
        [this] { releaseVoxHold(); });
    m_local->setPreviewHandlers(
        [this](const QString& path, QString& error) { return m_player->play(path, error); },
        [this] { m_player->stop(); });
    connect(m_player, &LocalWavPlayer::finished,
            m_local, &LocalVoiceKeyer::onPreviewFinished);
    connect(m_player, &LocalWavPlayer::muteRxRequested,
            this, &VoiceKeyerController::muteRxRequested);

    // The final TX monitor runs whenever the PC mic is captured, keyed or not.
    // Emitted on the audio thread; queued onto this object's thread.
    if (m_audio)
        connect(m_audio, &AudioEngine::txFinalMonitorPcmReady,
                m_local, &LocalVoiceKeyer::onMicPcm);

    wireTransmitter();

    // A swap deferred because a keyer was busy has to be finished by something.
    // Both keyers report when they free up, so the deferral resolves itself the
    // moment the operation in flight ends, rather than at the next unrelated
    // mode or slice edge.
    for (VoiceKeyer* k : {static_cast<VoiceKeyer*>(m_local),
                          static_cast<VoiceKeyer*>(&m_radio.dvkModel())}) {
        connect(k, &VoiceKeyer::statusChanged, this, [this] { refreshBinding(); });
        connect(k, &VoiceKeyer::admissionChanged, this, [this] { refreshBinding(); });
    }

    // Resolve once so the panel has a keyer before the first availability pass.
    m_bound = resolvedSource() == VoiceKeyerSource::Local
                  ? static_cast<VoiceKeyer*>(m_local)
                  : static_cast<VoiceKeyer*>(&m_radio.dvkModel());
}

VoiceKeyerController::~VoiceKeyerController() = default;

// Ask the radio to turn VOX off and wait for it to say it has.
//
// The command's reply is the confirmation, not a status echo: a FLEX-6500 on
// 4.2.20 answers `transmit set vox_enable=0` with R<seq>|0| in ~30 ms and then
// never mentions vox_enable in a transmit status again — it echoes dax changes
// but not VOX. Watching for a status change therefore waits forever. The
// reply's code is the radio's own answer, which is what Principle II asks for;
// the timer is only a backstop for a radio that never replies at all.
//
// The answer is matched by ticket, never by its value: an operator's own VOX
// click is answered too, and an answer to VOX ON must never start a recording.
void VoiceKeyerController::beginVoxHold(bool restoreAfter)
{
    m_restoreOwed = restoreAfter;
    m_voxHeldOff = true;
    emit voxHeldOffChanged(true);
    auto& tx = m_radio.transmitModel();
    m_holdTicket = tx.requestVoxEnable(false);
    m_voxMark = tx.voxIntentMark();
    m_voxConfirmTimer.start();
}

void VoiceKeyerController::onVoxAnswered(quint64 ticket, bool on, bool applied, bool current)
{
    if (ticket == 0)
        return;
    if (ticket == m_restoreTicket) {
        m_restoreTicket = 0;
        return;
    }
    if (ticket != m_holdTicket)
        return;   // somebody else's VOX change, or a wait already given up
    if (applied && current && !on) {
        finishVoxHold(HoldOutcome::Confirmed);
    } else if (!current) {
        finishVoxHold(HoldOutcome::Superseded);
    } else {
        finishVoxHold(HoldOutcome::Unconfirmed);
    }
}

void VoiceKeyerController::finishVoxHold(HoldOutcome outcome)
{
    m_voxConfirmTimer.stop();
    m_holdTicket = 0;
    if (outcome == HoldOutcome::Confirmed) {
        m_local->onRecordHoldReady();
        return;
    }
    m_voxHeldOff = false;
    emit voxHeldOffChanged(false);
    if (outcome == HoldOutcome::Superseded) {
        m_restoreOwed = false;
        // VOX was changed while the radio was answering, so the answer no
        // longer says whether it is off — and the change is the operator's to
        // keep, not this hold's to undo.
        m_local->onRecordHoldFailed(
            QStringLiteral("VOX was changed while recording was starting, so recording did "
                           "not start — try again."));
        return;
    }
    // The radio never said VOX was off, so recording would run with the
    // transmitter still armed. Put back what was asked for and say why.
    if (std::exchange(m_restoreOwed, false))
        restoreVox();
    m_local->onRecordHoldFailed(
        QStringLiteral("The radio did not confirm VOX is off, so recording would put it "
                       "on the air — turn VOX off on the radio and try again."));
}

// The recording ended, or STOP abandoned it while the radio was still
// answering: give the operator's VOX back. A late answer then finds no hold.
void VoiceKeyerController::releaseVoxHold()
{
    if (!m_voxHeldOff)
        return;
    m_voxConfirmTimer.stop();
    m_holdTicket = 0;
    m_voxHeldOff = false;
    if (std::exchange(m_restoreOwed, false))
        restoreVox();
    emit voxHeldOffChanged(false);
}

// Put VOX back the way the operator had it, unless VOX has been asked to
// change since this hold took it: that change is newer than the hold, so it
// stands. The radio's answer is tracked, so a REC started before it arrives
// holds again instead of reading the model's stale "off".
void VoiceKeyerController::restoreVox()
{
    auto& tx = m_radio.transmitModel();
    if (!m_radio.isConnected() || tx.voxIntentMark() != m_voxMark)
        return;
    m_restoreTicket = tx.requestVoxEnable(true);
}

// While a local recording runs, VOX turning on — the operator's click, or a
// status that reports it on — would key the radio on the operator's voice,
// whether or not this recording had to hold VOX off to start. The recording
// cannot be made safe again, so it ends, unsaved, and VOX is left as whoever
// turned it on wanted it. A radio with no VOX AetherSDR controls (the
// Hermes-Lite 2) has no VOX to key it, so its flag is not watched. (A
// FLEX-6500 does not repeat vox_enable in a status, so another client's
// change may never reach here.)
void VoiceKeyerController::stopRecordingIfVoxOn()
{
    if (m_local->status() != VoiceKeyer::Recording || m_holdTicket != 0
        || !m_radio.backendCapabilities().voxControl.has_value()
        || !m_radio.transmitModel().voxEnable())
        return;
    m_restoreOwed = false;
    if (std::exchange(m_voxHeldOff, false))
        emit voxHeldOffChanged(false);
    m_local->abandonRecording(
        QStringLiteral("VOX was turned on during the recording, which would key the radio, "
                       "so the recording was stopped and not saved."));
}

void VoiceKeyerController::wireTransmitter()
{
    // The route is parented to this controller; the transmitter to the route,
    // so neither outlives what it calls.
    m_route = new VoiceKeyerTxRoute(m_radio, m_audio, this);
    m_transmitter = new GeneratedAudioTransmitter(m_route, m_route);
    auto* tx = m_transmitter;
    auto& txModel = m_radio.transmitModel();
    connect(&m_radio, &RadioModel::txAudioStreamReady,
            tx, [tx](quint32) { tx->onStreamReady(); });
    connect(&m_radio, &RadioModel::txAudioFinished,
            tx, &GeneratedAudioTransmitter::onAudioFinished);
    connect(&m_radio, &RadioModel::radioTransmittingChanged,
            tx, &GeneratedAudioTransmitter::onRadioKeyed);
    connect(&m_radio, &RadioModel::radioTransmitConfirmed,
            tx, &GeneratedAudioTransmitter::onRadioKeyed);
    connect(&txModel, &TransmitModel::pttBlocked,
            tx, &GeneratedAudioTransmitter::onPttBlocked);
    connect(&txModel, &TransmitModel::moxChanged, tx, [tx](bool on) {
        if (!on)
            tx->onKeyReleased();
    });
    connect(&m_radio, &RadioModel::connectionStateChanged, tx, [tx](bool connected) {
        if (!connected)
            tx->abort(QStringLiteral("The radio disconnected."));
    });
    m_local->setTransmitter(tx);
}

VoiceKeyerSourceSetting VoiceKeyerController::sourceSetting() const
{
    return VoiceKeyerSettings::source();
}

void VoiceKeyerController::setSourceSetting(VoiceKeyerSourceSetting setting)
{
    VoiceKeyerSettings::setSource(setting);
    m_local->reload();
    refreshBinding();
}

VoiceKeyerSource VoiceKeyerController::resolvedSource() const
{
    return resolveVoiceKeyerSource(
        VoiceKeyerSettings::source(),
        m_radio.hasVoiceKeyer(),
        m_radio.licenseFeatureSeen(DvkModel::kLicenseFeature),
        m_radio.licenseFeatureEnabled(DvkModel::kLicenseFeature));
}

QString VoiceKeyerController::recordingsDir() const
{
    return VoiceKeyerSettings::recordingsDir();
}

void VoiceKeyerController::reloadLocalSlots()
{
    m_local->reload();
}

bool VoiceKeyerController::isTransmitting() const
{
    return m_transmitter && m_transmitter->isActive();
}

void VoiceKeyerController::abortTransmission(const QString& reason)
{
    if (m_transmitter)
        m_transmitter->abort(reason);
}

void VoiceKeyerController::refreshBinding()
{
    VoiceKeyer* wanted = resolvedSource() == VoiceKeyerSource::Local
                             ? static_cast<VoiceKeyer*>(m_local)
                             : static_cast<VoiceKeyer*>(&m_radio.dvkModel());
    if (wanted == m_bound) {
        m_deferralAnnounced = nullptr;
        return;
    }
    // Never pull the keyer out from under an operation: the one in flight must
    // keep the keyer that started it, so STOP targets what is actually running.
    //
    // canStartOperation(), not status(): a start already sent and still waiting
    // on the radio's echo reads as Idle, and a swap in that window would leave
    // STOP aimed at the other keyer while this one transmits. It also covers a
    // WAV transfer, which status() does not see at all.
    if (m_bound && !m_bound->canStartOperation()) {
        // Say so once: the operator picked a source and the panel is still
        // driving the other keyer until the operation in flight ends.
        if (m_deferralAnnounced != wanted) {
            m_deferralAnnounced = wanted;
            emit sourceChangeDeferred(wanted == static_cast<VoiceKeyer*>(m_local)
                                          ? VoiceKeyerSource::Local
                                          : VoiceKeyerSource::Radio);
        }
        return;
    }
    m_deferralAnnounced = nullptr;
    m_bound = wanted;
    emit boundKeyerChanged(m_bound);
}

}  // namespace AetherSDR
