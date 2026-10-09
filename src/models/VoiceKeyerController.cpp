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
    connect(&m_voxConfirmTimer, &QTimer::timeout, this, [this] { finishVoxHold(false); });

    m_local->setRecordGuards(
        [this]() -> bool {
            if (!shouldHoldVoxOff(m_radio.isConnected(),
                                  m_radio.transmitModel().voxEnable()))
                return true;   // nothing to hold, so nothing to wait for
            beginVoxHold();
            return false;      // recording waits for the radio's answer
        },
        [this] {
            if (!m_voxHeldOff)
                return;
            m_voxHeldOff = false;
            restoreVox();
            emit voxHeldOffChanged(false);
        });
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
void VoiceKeyerController::beginVoxHold()
{
    m_voxHeldOff = true;
    emit voxHeldOffChanged(true);
    connect(&m_radio, &RadioModel::connectionStateChanged, this, [this](bool connected) {
        if (!connected && m_voxConfirmTimer.isActive())
            finishVoxHold(false);
    });
    auto& tx = m_radio.transmitModel();
    m_voxConfirmWatch = connect(&tx, &TransmitModel::voxEnableAcknowledged, this,
                                [this](bool applied) {
        if (m_voxConfirmTimer.isActive())
            finishVoxHold(applied);
    });
    m_voxConfirmTimer.start();
    tx.requestVoxEnable(false);
}

// Put VOX back the way the operator had it, and reflect it only once the radio
// has agreed — the same reason beginVoxHold() holds the reply.
void VoiceKeyerController::restoreVox()
{
    m_radio.transmitModel().requestVoxEnable(true);
}

void VoiceKeyerController::finishVoxHold(bool confirmed)
{
    m_voxConfirmTimer.stop();
    if (m_voxConfirmWatch) {
        disconnect(m_voxConfirmWatch);
        m_voxConfirmWatch = {};
    }
    if (confirmed) {
        m_local->onRecordHoldReady();
        return;
    }
    // The radio never said VOX was off, so recording would run with the
    // transmitter still armed. Put back what was asked for and say why.
    m_voxHeldOff = false;
    emit voxHeldOffChanged(false);
    if (m_radio.isConnected())
        restoreVox();
    m_local->onRecordHoldFailed(
        QStringLiteral("The radio did not confirm VOX is off, so recording would put it "
                       "on the air — turn VOX off on the radio and try again."));
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
