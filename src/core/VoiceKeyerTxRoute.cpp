#include "VoiceKeyerTxRoute.h"

#include "core/AudioEngine.h"
#include "models/ModeVocabulary.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

namespace AetherSDR {

VoiceKeyerTxRoute::VoiceKeyerTxRoute(RadioModel& radio, AudioEngine* audio, QObject* parent)
    : QObject(parent), m_radio(radio), m_audio(audio) {}

VoiceKeyerTxRoute::~VoiceKeyerTxRoute() = default;

QString VoiceKeyerTxRoute::startRefusal() const
{
    if (!m_radio.isConnected() || !m_audio)
        return QStringLiteral("Connect to a radio to transmit.");
    if (!m_radio.backendCapabilities().canTransmit)
        return QStringLiteral("This radio cannot transmit.");
    SliceModel* s = m_radio.txSlice();
    if (!s)
        return QStringLiteral("No transmit slice is assigned.");
    if (!isVoiceMode(s->mode()))
        return QStringLiteral("The transmit slice is in %1 — voice keyer playback needs a "
                              "voice mode.").arg(s->mode());
    if (m_radio.transmitModel().isTransmitting())
        return QStringLiteral("The radio is already transmitting.");
    // In a voice mode DAX TX mode is only on while another generated source
    // (TCI, WSPR, AetherModem) owns the transmit audio.
    if (m_audio->isDaxTxMode())
        return QStringLiteral("Another application is already sending transmit audio.");
    return {};
}

bool VoiceKeyerTxRoute::needsStream() const
{
    const RadioCapabilities caps = m_radio.backendCapabilities();
    return !(caps.hostModulates || caps.takesTxAudioOverSeam);
}

bool VoiceKeyerTxRoute::streamReady() const
{
    return m_audio && m_audio->txStreamId() != 0;
}

bool VoiceKeyerTxRoute::requestStream()
{
    return m_radio.ensureDaxTxStream(DaxTxRequestReason::ClientVoiceKeyerTx);
}

void VoiceKeyerTxRoute::claimAudioPath()
{
    // Local DAX TX mode keeps the mic off the wire on every family;
    // `transmit dax` only means something to a radio with its own modulator
    // input choice (see Ax25HfPacketDecodeDialog).
    if (m_audio) {
        m_prevAudioDax = m_audio->isDaxTxMode();
        m_audio->setDaxTxMode(true);
    }
    m_restoreTransmitDax = needsStream();
    if (m_restoreTransmitDax) {
        auto& tx = m_radio.transmitModel();
        m_prevTransmitDax = tx.daxOn();
        tx.setDax(true);
    }
}

void VoiceKeyerTxRoute::releaseAudioPath()
{
    if (m_restoreTransmitDax)
        m_radio.transmitModel().setDax(m_prevTransmitDax);
    m_restoreTransmitDax = false;
    if (m_audio)
        m_audio->setDaxTxMode(m_prevAudioDax);
}

bool VoiceKeyerTxRoute::admitOperation()
{
    if (!m_producer.valid())
        m_producer = m_radio.registerTxProducer(this);
    m_request = m_producer.request();
    return m_request.valid();
}

bool VoiceKeyerTxRoute::keyOn()
{
    // The request was taken at the input boundary by admitOperation(); this
    // runs after the settle hop and only asks for the key against it.
    if (!m_request.valid())
        return false;
    if (!m_radio.requestProducerPttOn(m_request, TransmitModel::PttSource::Dax))
        return false;   // the request stays ours until keyOff() hands it back
    m_pttRequested = true;
    // Captured while the operation this request admitted is live; a context
    // taken before the key is not yet dispatchable.
    m_context = m_radio.captureTxMedia(m_request);
    if (!m_context.permitsDispatch(TxCoordinator::monotonicMs())) {
        // Keyed with nothing licensed to send: unkey here rather than hold the
        // transmitter up with a silent carrier.
        m_radio.requestProducerPttOff(m_request, TransmitModel::PttSource::Dax);
        m_pttRequested = false;
        m_request = {};
        m_context = {};
        return false;
    }
    return true;
}

void VoiceKeyerTxRoute::keyOff()
{
    // Unkey only if the key was actually asked for; an admitted request that
    // never reached PTT is still handed back below.
    if (m_pttRequested && m_request.valid())
        m_radio.requestProducerPttOff(m_request, TransmitModel::PttSource::Dax);
    m_pttRequested = false;
    // Blocks already queued hold their own copy of the context; clearing it
    // only stops NEW audio being licensed after the key is released.
    m_context = {};
    m_request = {};
}

bool VoiceKeyerTxRoute::isKeyed() const
{
    return m_radio.transmitModel().isTransmitting();
}

bool VoiceKeyerTxRoute::waitsForRadioPtt() const
{
    return m_radio.backendCapabilities().hasRadioPttReadback;
}

bool VoiceKeyerTxRoute::isRadioKeyed() const
{
    return m_radio.isRadioTransmitting();
}

void VoiceKeyerTxRoute::sendAudio(const QByteArray& float32Stereo24k)
{
    QPointer<AudioEngine> audio = m_audio;
    QMetaObject::invokeMethod(m_audio, [audio, pcm = float32Stereo24k, context = m_context] {
        if (audio)
            audio->sendModemTxAudio(pcm, context);
    }, Qt::QueuedConnection);
}

bool VoiceKeyerTxRoute::finishAudio(quint64 token)
{
    QPointer<AudioEngine> audio = m_audio;
    return QMetaObject::invokeMethod(m_audio, [audio, token, context = m_context] {
        if (audio)
            audio->finishModemTxAudio(token, context);
    }, Qt::QueuedConnection);
}

void VoiceKeyerTxRoute::clearAudio()
{
    if (m_audio)
        m_audio->clearTxAccumulators();   // self-marshals
}

}  // namespace AetherSDR
