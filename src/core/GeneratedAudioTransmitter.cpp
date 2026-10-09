#include "GeneratedAudioTransmitter.h"

#include "LogManager.h"

#include <algorithm>

namespace AetherSDR {

namespace {
constexpr qsizetype kBytesPerMs =
    GeneratedAudioTransmitter::kSampleRate * GeneratedAudioTransmitter::kFrameBytes / 1000;
}

GeneratedAudioTransmitter::GeneratedAudioTransmitter(TxAudioRoute* route, QObject* parent,
                                                     GeneratedAudioTiming timing)
    : QObject(parent)
    , m_route(route)
    , m_timing(timing)
{
    m_paceTimer.setTimerType(Qt::PreciseTimer);
    m_paceTimer.setInterval(std::max(1, m_timing.chunkMs));
    connect(&m_paceTimer, &QTimer::timeout, this, &GeneratedAudioTransmitter::paceTick);
}

int GeneratedAudioTransmitter::audioMs() const
{
    return int(m_pcm.size() / kBytesPerMs);
}

int GeneratedAudioTransmitter::sentMs() const
{
    return int(m_offset / kBytesPerMs);
}

void GeneratedAudioTransmitter::setPhase(Phase phase)
{
    if (m_phase == phase)
        return;
    m_phase = phase;
    emit phaseChanged(phase);
}

void GeneratedAudioTransmitter::after(int ms, void (GeneratedAudioTransmitter::*fn)())
{
    // Stamped with the transmission it belongs to, so a timer from one that
    // has ended can never act on the next.
    QTimer::singleShot(std::max(0, ms), this, [this, fn, gen = m_generation] {
        if (gen == m_generation && m_phase != Phase::Idle)
            (this->*fn)();
    });
}

bool GeneratedAudioTransmitter::start(const QByteArray& pcm, QString& error)
{
    if (isActive()) {
        error = QStringLiteral("A transmission is already in progress.");
        return false;
    }
    if (pcm.size() < kFrameBytes) {
        error = QStringLiteral("There is no audio to transmit.");
        return false;
    }
    if (!m_route) {
        error = QStringLiteral("No transmit route is available.");
        return false;
    }
    if (QString refusal = m_route->startRefusal(); !refusal.isEmpty()) {
        error = refusal;
        return false;
    }

    ++m_generation;
    m_pcm = pcm;
    m_pcm.truncate(m_pcm.size() - m_pcm.size() % kFrameBytes);
    m_offset = 0;
    m_claimed = false;
    m_keyRequested = false;
    m_admitted = false;

    // A refusal we can see synchronously comes first, so nothing is taken from
    // the coordinator on a path that never transmits.
    const bool wantsStream = m_route->needsStream() && !m_route->streamReady();
    if (wantsStream && !m_route->requestStream()) {
        m_pcm.clear();
        error = QStringLiteral("The radio's transmit audio stream could not be requested.");
        return false;
    }

    // Then, still before anything queued: the authority this transmission
    // carries is taken at the operator's edge, not 150 ms later on the settle
    // timer, nor after the stream wait below (TxCoordinator.h).
    if (!m_route->admitOperation()) {
        m_pcm.clear();
        error = QStringLiteral("The radio did not admit a transmit request.");
        return false;
    }
    m_admitted = true;

    if (wantsStream) {
        setPhase(Phase::WaitingForStream);
        qCInfo(lcAudio) << "GeneratedAudioTransmitter: waiting for the TX audio stream";
        QTimer::singleShot(m_timing.streamWaitMs, this, [this, gen = m_generation] {
            if (gen == m_generation && m_phase == Phase::WaitingForStream)
                abort(QStringLiteral("The radio's transmit audio stream did not arrive within %1 ms.")
                          .arg(m_timing.streamWaitMs));
        });
        return true;
    }
    beginKeying();
    return true;
}

void GeneratedAudioTransmitter::onStreamReady()
{
    if (m_phase == Phase::WaitingForStream)
        beginKeying();
}

void GeneratedAudioTransmitter::beginKeying()
{
    // The mic comes off the transmit path before the key goes down, so no
    // live room audio leads the message.
    m_route->claimAudioPath();
    m_claimed = true;
    setPhase(Phase::Settling);
    qCInfo(lcAudio).noquote()
        << QStringLiteral("GeneratedAudioTransmitter: keying for %1 ms of audio (%2)")
               .arg(audioMs())
               .arg(m_route->waitsForRadioPtt() ? QStringLiteral("waits for radio PTT")
                                                : QStringLiteral("command-edge PTT"));
    after(m_timing.settleMs, &GeneratedAudioTransmitter::keyAfterSettle);
}

void GeneratedAudioTransmitter::keyAfterSettle()
{
    if (m_phase != Phase::Settling)
        return;
    setPhase(Phase::WaitingForPtt);
    m_keyRequested = true;
    if (!m_route->keyOn()) {
        // The route never acquired transmit authority. finish() unkeys only a
        // route that reports itself keyed, so this hands the audio path back
        // without a keyOff the radio never needed.
        abort(QStringLiteral("The radio refused the transmit request."));
        return;
    }
    if (m_phase != Phase::WaitingForPtt)
        return;   // refused synchronously (pttBlocked) and already finished

    if (m_route->waitsForRadioPtt()) {
        if (m_route->isRadioKeyed()) {
            startLeadIn();
            return;
        }
        QTimer::singleShot(m_timing.pttConfirmMs, this, [this, gen = m_generation] {
            if (gen == m_generation && m_phase == Phase::WaitingForPtt)
                abort(QStringLiteral("The radio did not confirm PTT within %1 ms.")
                          .arg(m_timing.pttConfirmMs));
        });
        return;
    }
    if (!m_route->isKeyed()) {
        abort(QStringLiteral("PTT did not engage."));
        return;
    }
    startLeadIn();
}

void GeneratedAudioTransmitter::onRadioKeyed(bool keyed)
{
    if (keyed && m_phase == Phase::WaitingForPtt && m_route->waitsForRadioPtt())
        startLeadIn();
}

void GeneratedAudioTransmitter::onPttBlocked(const QString& message)
{
    // pttBlocked is not addressed: a TUNE refused mid-message is someone
    // else's. Only the answer to our own key request ends this transmission.
    if (m_phase == Phase::WaitingForPtt)
        abort(message.isEmpty() ? QStringLiteral("PTT was blocked.") : message);
}

void GeneratedAudioTransmitter::onKeyReleased()
{
    if (m_keyRequested && m_phase != Phase::Idle)
        finish(Outcome::Stopped, QStringLiteral("Transmit was released."));
}

void GeneratedAudioTransmitter::startLeadIn()
{
    setPhase(Phase::LeadIn);
    after(m_timing.leadMs, &GeneratedAudioTransmitter::startSending);
}

void GeneratedAudioTransmitter::startSending()
{
    if (m_phase != Phase::LeadIn)
        return;
    setPhase(Phase::Sending);
    m_paceClock.restart();
    paceTick();
    if (m_phase == Phase::Sending)
        m_paceTimer.start();
}

void GeneratedAudioTransmitter::paceTick()
{
    if (m_phase != Phase::Sending)
        return;

    if (m_offset >= m_pcm.size()) {
        m_paceTimer.stop();
        setPhase(Phase::Draining);
        // Unkey only after the audio has left: the barrier is queued behind
        // every block, then the backend reports what it still holds.
        if (!m_route->finishAudio(m_generation)) {
            abort(QStringLiteral("Could not queue the end-of-audio barrier."));
            return;
        }
        QTimer::singleShot(m_timing.finishWaitMs, this, [this, gen = m_generation] {
            if (gen == m_generation && m_phase == Phase::Draining)
                abort(QStringLiteral("The end of the transmitted audio was not confirmed."));
        });
        return;
    }

    // Catch-up pacing (AetherModem's): keep the radio fed to real time plus a
    // cushion. A late tick sends more; an early one sends nothing.
    const qsizetype target = kBytesPerMs * (m_paceClock.elapsed() + m_timing.leadBufferMs);
    if (target <= m_offset)
        return;
    qsizetype n = std::min(target - m_offset, m_pcm.size() - m_offset);
    n -= n % kFrameBytes;
    if (n <= 0)
        return;
    const QByteArray chunk = m_pcm.mid(m_offset, n);
    m_offset += n;
    m_route->sendAudio(chunk);
}

void GeneratedAudioTransmitter::onAudioFinished(quint64 token, int drainMs)
{
    if (m_phase != Phase::Draining || token != m_generation)
        return;
    // Leaving Draining stands the barrier watchdog down.
    setPhase(Phase::Tail);
    const int holdMs = m_timing.tailMs + std::max(0, drainMs);
    QTimer::singleShot(holdMs, this, [this, gen = m_generation] {
        if (gen == m_generation && m_phase == Phase::Tail)
            finish(Outcome::Completed, QString());
    });
}

void GeneratedAudioTransmitter::stop()
{
    finish(Outcome::Stopped, QString());
}

void GeneratedAudioTransmitter::abort(const QString& reason)
{
    finish(Outcome::Failed, reason);
}

void GeneratedAudioTransmitter::finish(Outcome outcome, const QString& reason)
{
    if (m_phase == Phase::Idle)
        return;
    m_paceTimer.stop();
    ++m_generation;   // every pending timer of this transmission stands down
    // Idle BEFORE unkeying: our own release must not read as someone else's.
    m_phase = Phase::Idle;

    // Whatever the radio echoed, an admitted request is handed back here: the
    // admission is what took it, so the admission is what releases it. Testing
    // the key instead would leak a request on every path that ends before the
    // radio shows keyed — a stop or timeout waiting for PTT, or a key that
    // never engaged.
    if (m_admitted)
        m_route->keyOff();
    if (m_claimed)
        m_route->releaseAudioPath();
    m_route->clearAudio();

    const int sent = sentMs();
    m_pcm.clear();
    m_offset = 0;
    m_claimed = false;
    m_keyRequested = false;
    m_admitted = false;

    if (outcome == Outcome::Failed)
        qCWarning(lcAudio).noquote() << "GeneratedAudioTransmitter: aborted:" << reason;
    else
        qCInfo(lcAudio).noquote() << "GeneratedAudioTransmitter:"
                                  << (outcome == Outcome::Completed ? "completed" : "stopped")
                                  << "after" << sent << "ms";
    emit phaseChanged(Phase::Idle);
    emit finished(outcome, reason);
}

} // namespace AetherSDR
