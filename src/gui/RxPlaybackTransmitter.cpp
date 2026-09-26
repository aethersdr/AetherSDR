#include "RxPlaybackTransmitter.h"

#include "core/AudioEngine.h"
#include "core/LogManager.h"
// DaxTxRequestReason arrives through RadioModel.h, whose vendor coupling is
// already on the engine-boundary baseline; this file adds none of its own.
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QTimer>

#include <algorithm>

namespace AetherSDR {

namespace {
// The engine's internal rate; the recorder's WAV is 24 kHz stereo int16 and
// the modem route takes stereo float32 at the same rate.
constexpr int kSampleRate = 24000;
constexpr qsizetype kFrameBytes = 2 * static_cast<qsizetype>(sizeof(float));
// Same schedule the AX.25 modem uses on this route: let DAX TX settle before
// keying, give the radio a lead before sample zero, pace in 20 ms chunks and
// keep a cushion ahead of real time so a GUI stall does not starve the FIFO,
// then hold the carrier for the backend's drain plus a short tail.
constexpr int kDaxSettleMs = 150;
constexpr int kLeadMs = 200;
constexpr int kChunkMs = 20;
constexpr int kLeadBufferMs = 120;
constexpr int kTailMs = 150;
constexpr int kStreamWaitMs = 5000;
constexpr int kPttConfirmMs = 2000;

// See m_generation in the header: one counter for every session in the
// process, started where no per-object counter will ever reach.
quint64 nextSessionToken()
{
    static quint64 token = quint64(1) << 40;
    return ++token;
}
} // namespace

RxPlaybackTransmitter::RxPlaybackTransmitter(RadioModel* radio, AudioEngine* audio,
                                             QObject* parent)
    : QObject(parent)
    , m_radio(radio)
    , m_audio(audio)
{
    m_pacer = new QTimer(this);
    m_pacer->setInterval(kChunkMs);
    connect(m_pacer, &QTimer::timeout, this, &RxPlaybackTransmitter::pace);

    if (!m_radio) return;
    connect(m_radio, &RadioModel::txAudioStreamReady, this, [this](quint32) {
        if (m_pendingStream) beginWhenReady();
    });
    connect(m_radio, &RadioModel::txAudioFinished,
            this, &RxPlaybackTransmitter::onTxAudioFinished);
    connect(m_radio, &RadioModel::connectionStateChanged, this, [this](bool connected) {
        if (!connected && active()) finish(true, tr("the radio disconnected"));
    });
    // Any producer's block ends this session too. Deliberately unfiltered,
    // as the packet dialog's is: a block is the radio saying "not now", and
    // the safe reading of that while we hold a request is to let go.
    connect(&m_radio->transmitModel(), &TransmitModel::pttBlocked,
            this, [this](const QString& message) {
        if (active()) finish(true, tr("PTT blocked: %1").arg(message));
    });
    // The operator unkeying from anywhere else -- MOX button, footswitch, a
    // cancel -- ends the session rather than leaving a pacer feeding a
    // transmitter that is no longer ours. Our own release happens after
    // m_active clears, so it never re-enters here.
    connect(&m_radio->transmitModel(), &TransmitModel::transmittingChanged,
            this, [this](bool tx) {
        if (!tx && m_active && m_audioStartArmed)
            finish(true, tr("the transmitter was unkeyed"));
    });
}

RxPlaybackTransmitter::~RxPlaybackTransmitter()
{
    if (active()) finish(true, QStringLiteral("shutting down"));
}

bool RxPlaybackTransmitter::bypassesDax() const
{
    // Same question the AX.25 modem asks: does this radio's TX audio need a
    // DAX stream at all? A host-modulating backend (HL2) and a seam backend
    // (Icom) both take the direct route.
    if (!m_radio) return false;
    const RadioCapabilities caps = m_radio->backendCapabilities();
    return caps.hostModulates || caps.takesTxAudioOverSeam;
}

qsizetype RxPlaybackTransmitter::bytesForSeconds(int seconds)
{
    return static_cast<qsizetype>(std::max(0, seconds)) * kSampleRate * kFrameBytes;
}

QAudioFormat RxPlaybackTransmitter::wireFormat()
{
    QAudioFormat fmt;
    fmt.setSampleRate(kSampleRate);
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Float);
    return fmt;
}

bool RxPlaybackTransmitter::start(const QByteArray& pcm, SliceModel* slice,
                                  const TxCoordinator::Request& input, QString* whyNot)
{
    const auto refuse = [whyNot](const QString& why) {
        if (whyNot) *whyNot = why;
        return false;
    };
    if (active())            return refuse(tr("a transmit playback is already running"));
    if (!m_radio || !m_audio) return refuse(tr("the radio or audio engine is gone"));
    if (!m_radio->isConnected()) return refuse(tr("no radio is connected"));
    if (!m_radio->backendCapabilities().canTransmit)
        return refuse(tr("this radio cannot transmit"));
    if (!input.valid())      return refuse(tr("the transmit request is not valid"));
    if (pcm.size() < kFrameBytes) return refuse(tr("the recording is empty"));

    m_generation = nextSessionToken();
    m_request = input;
    m_pcm = pcm;
    m_offset = 0;
    m_audioStartArmed = false;
    m_awaitingFinish = false;

    // "Over the active slice": the radio modulates whichever slice is TX, so
    // the active one is made TX first if it is not already.
    if (slice && !slice->isTxSlice()) {
        qCInfo(lcAudio) << "RxPlaybackTransmitter: selecting slice" << slice->sliceId()
                        << "for transmit playback";
        slice->setTxSlice(true);
    }

    qCInfo(lcAudio).noquote()
        << QStringLiteral("RxPlaybackTransmitter: start (%1 s)")
               .arg(static_cast<double>(m_pcm.size()) / (kFrameBytes * kSampleRate), 0, 'f', 1);

    if (!bypassesDax() && m_audio->txStreamId() == 0) {
        // The create reply is asynchronous and its failure only logs, so a
        // timeout is what ends this wait on a radio with no DAX transport.
        m_pendingStream = true;
        emit activeChanged(true);
        // From here on the session reports its own end through finished(),
        // so a failure is not also handed back as a refusal.
        if (!m_radio->ensureDaxTxStream(DaxTxRequestReason::RxPlaybackTx)) {
            finish(true, tr("DAX TX stream policy rejected stream creation"));
            return true;
        }
        QTimer::singleShot(kStreamWaitMs, this, [this, gen = m_generation] {
            if (m_pendingStream && gen == m_generation)
                finish(true, tr("DAX TX stream did not arrive within %1 ms")
                                 .arg(kStreamWaitMs));
        });
        return true;
    }

    beginWhenReady();
    return true;
}

void RxPlaybackTransmitter::abort(const QString& reason)
{
    if (active()) finish(true, reason);
}

void RxPlaybackTransmitter::beginWhenReady()
{
    if (m_pcm.isEmpty()) return;
    if (!m_audio || !m_radio) {
        finish(true, tr("audio engine or radio model disappeared before TX"));
        return;
    }
    if (!m_request.valid()) {
        finish(true, tr("the transmit request is no longer valid"));
        return;
    }
    const bool bypass = bypassesDax();
    if (!bypass && m_audio->txStreamId() == 0) {
        m_pendingStream = true;
        return;
    }

    const bool wasActive = active();
    m_pendingStream = false;
    m_active = true;
    if (!wasActive) emit activeChanged(true);

    // Local DAX TX mode keeps the microphone off the wire while we modulate;
    // `transmit dax` tells a FLEX to take its modulator input from the DAX
    // stream, and only a radio with a DAX stream has that choice to make.
    auto& txModel = m_radio->transmitModel();
    m_previousAudioDaxMode = m_audio->isDaxTxMode();
    m_restoreAudioDaxMode = true;
    m_audio->setDaxTxMode(true);
    if (!bypass) {
        m_previousTransmitDax = txModel.daxOn();
        m_restoreTransmitDax = true;
        txModel.setDax(true);
    }

    QTimer::singleShot(kDaxSettleMs, this, [this, request = m_request] {
        if (!m_active || !request.sameRequest(m_request)) return;
        if (!m_radio) {
            finish(true, tr("radio model disappeared before PTT"));
            return;
        }
        // A backend whose keyed state is the radio's own readback (Icom) gets
        // sample zero only after that readback; the command edge is intent.
        const bool waitsForRadioPtt = m_radio->backendCapabilities().hasRadioPttReadback;
        const quint64 generation = m_generation;

        disconnectPttConfirmation();
        if (waitsForRadioPtt) {
            const auto confirmed = [this, generation](bool transmitting) {
                if (!transmitting || !m_active || generation != m_generation) return;
                disconnectPttConfirmation();
                startAudioAfterPtt();
            };
            m_pttConfirm = connect(m_radio, &RadioModel::radioTransmittingChanged,
                                   this, confirmed);
            m_pttConfirmed = connect(m_radio, &RadioModel::radioTransmitConfirmed,
                                     this, confirmed);
        }

        if (!m_radio->requestProducerPttOn(request, TransmitModel::PttSource::Dax)) {
            finish(true, tr("PTT request was refused"));
            return;
        }
        m_context = m_radio->captureTxMedia(request);
        if (!m_active) return;
        if (waitsForRadioPtt) {
            if (m_radio->isRadioTransmitting()) {
                disconnectPttConfirmation();
                startAudioAfterPtt();
                return;
            }
            QTimer::singleShot(kPttConfirmMs, this, [this, generation] {
                if (!m_active || m_audioStartArmed || generation != m_generation) return;
                finish(true, tr("radio did not confirm PTT within %1 ms").arg(kPttConfirmMs));
            });
            return;
        }
        if (!m_radio->transmitModel().isTransmitting()) {
            finish(true, tr("PTT did not engage"));
            return;
        }
        startAudioAfterPtt();
    });
}

void RxPlaybackTransmitter::startAudioAfterPtt()
{
    if (!m_active || m_audioStartArmed) return;
    m_audioStartArmed = true;
    const quint64 generation = m_generation;
    QTimer::singleShot(kLeadMs, this, [this, generation] {
        if (!m_active || generation != m_generation) return;
        m_paceClock.restart();
        pace();
        if (m_active) m_pacer->start();
    });
}

void RxPlaybackTransmitter::pace()
{
    if (!m_active || !m_audio) return;

    if (m_offset >= m_pcm.size()) {
        m_pacer->stop();
        if (m_awaitingFinish) return;
        m_awaitingFinish = true;
        // Queue the completion barrier behind the last block; the backend
        // answers through RadioModel::txAudioFinished with its drain time.
        const quint64 generation = m_generation;
        QPointer<AudioEngine> audio = m_audio;
        const bool queued = QMetaObject::invokeMethod(
            m_audio, [audio, generation, context = m_context] {
                if (audio) audio->finishModemTxAudio(generation, context);
            }, Qt::QueuedConnection);
        if (!queued) finish(true, tr("could not queue the TX-audio completion barrier"));
        return;
    }

    // Catch-up pacing: keep the radio's TX FIFO filled to (real time elapsed
    // + kLeadBufferMs). A late tick ships a larger chunk to refill the
    // cushion; when already ahead, nothing goes until real time catches up.
    const qint64 nowMs = m_paceClock.isValid() ? m_paceClock.elapsed() : 0;
    const qsizetype bytesPerMs = static_cast<qsizetype>(kSampleRate) * kFrameBytes / 1000;
    const qsizetype targetBytes = bytesPerMs * (nowMs + kLeadBufferMs);
    if (targetBytes <= m_offset) return;
    qsizetype sendBytes = std::min<qsizetype>(targetBytes - m_offset, m_pcm.size() - m_offset);
    sendBytes -= sendBytes % kFrameBytes;
    if (sendBytes <= 0) return;
    const QByteArray chunk = m_pcm.mid(m_offset, sendBytes);
    m_offset += sendBytes;

    QPointer<AudioEngine> audio = m_audio;
    QMetaObject::invokeMethod(m_audio, [audio, chunk, context = m_context] {
        if (audio) audio->sendModemTxAudio(chunk, context);
    }, Qt::QueuedConnection);
}

void RxPlaybackTransmitter::onTxAudioFinished(quint64 token, int drainMs)
{
    if (!m_active || !m_awaitingFinish || token != m_generation) return;
    m_awaitingFinish = false;
    const int unkeyDelayMs = kTailMs + std::max(0, drainMs);
    QTimer::singleShot(unkeyDelayMs, this, [this, token] {
        if (m_active && token == m_generation)
            finish(false, tr("transmit playback complete"));
    });
}

void RxPlaybackTransmitter::disconnectPttConfirmation()
{
    if (m_pttConfirm)   { disconnect(m_pttConfirm);   m_pttConfirm = {}; }
    if (m_pttConfirmed) { disconnect(m_pttConfirmed); m_pttConfirmed = {}; }
}

void RxPlaybackTransmitter::finish(bool aborted, const QString& reason)
{
    m_pacer->stop();
    disconnectPttConfirmation();

    const bool wasActive = active();
    m_active = false;
    m_audioStartArmed = false;
    m_awaitingFinish = false;
    m_pendingStream = false;

    if (m_radio) {
        auto& txModel = m_radio->transmitModel();
        if (aborted) m_radio->abortProducerPtt(m_request, TransmitModel::PttSource::Dax);
        else         m_radio->requestProducerPttOff(m_request, TransmitModel::PttSource::Dax);
        if (m_restoreTransmitDax) txModel.setDax(m_previousTransmitDax);
    }
    m_request = {};
    if (m_audio) {
        if (m_restoreAudioDaxMode) m_audio->setDaxTxMode(m_previousAudioDaxMode);
        m_audio->discardTxMedia(m_context);
    }
    m_context = {};
    m_pcm.clear();
    m_offset = 0;
    m_restoreAudioDaxMode = false;
    m_restoreTransmitDax = false;

    if (wasActive) {
        qCInfo(lcAudio).noquote()
            << QStringLiteral("RxPlaybackTransmitter: %1: %2")
                   .arg(aborted ? QStringLiteral("aborted") : QStringLiteral("finished"), reason);
        emit activeChanged(false);
        emit finished(aborted, reason);
    }
}

} // namespace AetherSDR
