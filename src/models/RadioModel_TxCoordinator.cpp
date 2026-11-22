#include "RadioModel.h"
#include "HostCwTextEngine.h"
#include "ModeVocabulary.h"
#include "TxController.h"
#include "core/CwxLocalKeyer.h"
#include "core/LogManager.h"
#include "core/SerialPortController.h"

#include <QPointer>
#include <QScopeGuard>
#include <QThread>
#include <algorithm>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace AetherSDR {

qint64 RadioModel::txMonotonicMs()
{
    return TxCoordinator::monotonicMs();
}

TxCoordinator::Producer RadioModel::registerTxProducer(QObject* lifetime, bool continuousMicrophone)
{
    if (!lifetime || QThread::currentThread() != thread()) {
        return {};
    }
    const TxCoordinator::Producer producer = m_txCoordinator.registerProducer(m_desktopTxActor, continuousMicrophone);
    connect(lifetime, &QObject::destroyed, this, [producer] {
        producer.invalidate();
    }, Qt::DirectConnection); // atomic invalidation; never touches a model
    return producer;
}

TxCoordinator::Producer RadioModel::registerTxProducer()
{
    return QThread::currentThread() == thread()
        ? m_txCoordinator.registerProducer(m_desktopTxActor) : TxCoordinator::Producer{};
}

TxCoordinator::Producer RadioModel::registerTxProducerFor(const TxCoordinator::Request& input)
{
    return QThread::currentThread() == thread() && m_txCoordinator.acceptsRequest(input)
        ? m_txCoordinator.registerProducer(m_txCoordinator.requestActor(input)) : TxCoordinator::Producer{};
}

std::shared_ptr<TxController> RadioModel::localTxController()
{
    if (QThread::currentThread() != thread()) {
        return {};
    }
    if (!m_localTxController || !m_localTxController->valid()) {
        m_localTxController = std::make_shared<TxController>(this);
    }
    return m_localTxController;
}

bool RadioModel::canBindGrantedPtt(const TxCoordinator::Request& request,
                                  const TxCoordinator::Operation& operation) const
{
    if (QThread::currentThread() != thread() || m_txSessionClosing || !m_backend
        || !m_txCoordinator.isIndependentRequest(request)) {
        return false;
    }
    const TxCoordinator::Intent intent = m_txCoordinator.requestIntent(request);
    return intent.pending() && intent.isActivity(TxActivity::Mox)
        && m_txCoordinator.requestOperation(request).sameAuthority(operation)
        && operation.permitsDispatch(txMonotonicMs());
}

void RadioModel::setTxProducerAdmissionObserver(const TxCoordinator::Producer& producer,
                                               std::function<void()> observer)
{
    m_txCoordinator.setProducerAdmissionObserver(producer, std::move(observer));
}

TxCoordinator::Context RadioModel::captureTxMedia(const TxCoordinator::Request& request) const
{
    if (QThread::currentThread() != thread() || m_txSessionClosing || !m_backend) {
        return {};
    }
    return m_txCoordinator.mediaContext(request);
}

TxCoordinator::Context RadioModel::captureTxMedia(const TxCoordinator::Producer& producer) const
{
    if (QThread::currentThread() != thread() || m_txSessionClosing || !m_backend) {
        return {};
    }
    return m_txCoordinator.mediaContext(producer, m_txOperation);
}

bool RadioModel::beginLocalTxActivity(TxActivity activity)
{
    return beginTxActivity(activity, nullptr);
}

bool RadioModel::producerRequestHasWork(const TxCoordinator::Request& request) const
{
    const TxCoordinator::Intent intent = m_txCoordinator.requestIntent(request);
    const TxCoordinator::Operation operation = m_txCoordinator.requestOperation(request);
    if (!operation.permitsCleanup() || !operation.sameOperation(m_txOperation)) {
        return false;
    }
    if (intent.pending()) { return true; }
    if ((intent.isActivity(TxActivity::Mox) || intent.isActivity(TxActivity::CwPtt))
        && hasOtherPttHolds(operation, intent)) {
        // This contributor has handed back its hold. Any remaining carrier
        // belongs to a compatible contributor, not to its finished watchdog.
        return false;
    }
    return m_transmitModel.isTransmitting()
        || m_transmitModel.isMox() || m_transmitModel.isTuning();
}

bool RadioModel::txProducerHasWork(const TxCoordinator::Producer& producer) const
{
    const auto requests = m_txCoordinator.producerRequests(producer);
    return std::any_of(requests.begin(), requests.end(), [this](const auto& request) {
        return producerRequestHasWork(request);
    });
}

void RadioModel::abortTxProducerInputs(const TxCoordinator::Producer& producer,
                                      TransmitModel::PttSource source)
{
    const QPointer<RadioModel> radio(this);
    const auto requests = m_txCoordinator.producerRequests(producer);
    // Generators first, PTT last. Includes derived sequencer elements whose
    // owning UI/input callback has already returned; the engine retains each
    // admitted request until its terminal cleanup is consumed.
    for (const TxActivity activity : {TxActivity::Cwx, TxActivity::CwKey, TxActivity::CwPtt,
                                      TxActivity::Atu, TxActivity::Tune, TxActivity::Mox}) {
        for (const auto& request : requests) {
            if (!radio) {
                return;
            }
            if (!m_txCoordinator.requestIntent(request).isActivity(activity)) {
                continue;
            }
            switch (activity) {
            case TxActivity::Cwx: abortProducerCwx(request); break;
            case TxActivity::CwKey: (void)requestProducerCw(request, false); break;
            case TxActivity::CwPtt: (void)requestProducerCw(request, false, true); break;
            case TxActivity::Atu: (void)requestProducerAtu(request, false); break;
            case TxActivity::Tune: (void)requestProducerTune(request, false); break;
            case TxActivity::Mox: abortProducerPtt(request, source); break;
            case TxActivity::VoiceKeyer: case TxActivity::Navtex: break;
            }
        }
    }
}

bool RadioModel::requestProducerPttOn(const TxCoordinator::Request& request,
                                     TransmitModel::PttSource source)
{
    if (QThread::currentThread() != thread()) {
        return false;
    }
    bool engaged = false;
    ++m_txKeyDownDepth;
    const auto keyDown = qScopeGuard([this] { --m_txKeyDownDepth; });
    m_transmitModel.requestPttOn(source, [this, request]() -> TransmitModel::KeyingPermit {
        if (!beginTxActivity(TxActivity::Mox, &request)) {
            return {};
        }
        const TxCoordinator::Operation operation = m_txCoordinator.requestOperation(request);
        return [operation] { return operation.permitsDispatch(txMonotonicMs()); };
    }, [this, request, source, &engaged] {
        engaged = setProducerTransmit(request, true, source);
    });
    if (!engaged) {
        // Preflight/admission can reenter. A refusal is not a held request
        // that a later callback may opportunistically turn into transmit.
        (void)setProducerTransmit(request, false, source);
    }
    return engaged;
}

void RadioModel::requestProducerPttOff(const TxCoordinator::Request& request,
                                      TransmitModel::PttSource source)
{
    if (QThread::currentThread() != thread()) {
        return;
    }
    noteTxTimeoutRelease(request);
    const TxCoordinator::Intent bound = m_txCoordinator.requestIntent(request);
    if (!bound.isActivity(TxActivity::Mox)) {
        if (!bound.pending()) {
            (void)m_txCoordinator.closeRequest(request);
        }
        return;
    }
    const TxCoordinator::Intent intent = m_txCoordinator.closeRequest(request);
    if (!intent.pending()) {
        return;
    }
    const TxCoordinator::Operation operation = m_txCoordinator.requestOperation(request);
    if (hasOtherPttHolds(operation, intent)) {
        endLocalTxActivity(intent);
        m_txRequested = activeTxActivities() & static_cast<unsigned>(TxActivity::Mox);
        return;
    }
    const QPointer<RadioModel> receiver(this);
    m_transmitModel.requestPttOff(source, {
        [operation, intent] { return operation.permitsCleanup() && intent.pending(); },
        [receiver, request, source] {
            if (receiver) {
                receiver->setTransmitImpl(false, source, &request, true);
            }
        },
        [receiver, intent] {
            if (receiver) {
                receiver->endLocalTxActivity(intent);
            }
        }});
}

void RadioModel::connectSerialPttInput(SerialPortController* source)
{
    // Both edges go through the normal PTT paths so the footswitch gets the
    // same tails as MOX: Quindar intro/outro, RADE end-of-over, TX off delay.
    connect(source, &SerialPortController::externalPttChanged,
            this, [this](bool active, const TxCoordinator::Request& input) {
        if (active) {
            (void)requestProducerPttOn(input, TransmitModel::PttSource::Mox);
        } else {
            noteTxInputReleased(input);
            requestProducerPttOff(input, TransmitModel::PttSource::Mox);
        }
    });
}

void RadioModel::abortProducerPtt(const TxCoordinator::Request& request,
                                 TransmitModel::PttSource source)
{
    if (QThread::currentThread() != thread()) {
        return;
    }
    // Abandoning a key it held is a release; noteTxTimeoutRelease() skips
    // the teardown of a key the latch just refused.
    noteTxTimeoutRelease(request);
    const TxCoordinator::Intent bound = m_txCoordinator.requestIntent(request);
    if (!bound.isActivity(TxActivity::Mox)) {
        if (!bound.pending()) {
            (void)m_txCoordinator.closeRequest(request);
        }
        return;
    }
    (void)m_txCoordinator.closeRequest(request);
    const TxCoordinator::Intent intent = m_txCoordinator.requestIntent(request);
    if (intent.pending()) {
        (void)setTransmitImpl(false, source, &request, true);
        return;
    }
    const TxCoordinator::Operation operation = m_txCoordinator.requestOperation(request);
    if (operation.permitsCleanup() && !m_txCoordinator.hasOtherIntents(operation, {})) {
        // Retry only this operation's one-way stop after a late radio edge.
        // A new contributor or connection can never inherit this cleanup.
        requestTransmitStop(operation);
    }
}

bool RadioModel::beginTxActivity(TxActivity activity, const TxCoordinator::Request* request,
                                 bool askKeyObjection)
{
    if (m_txInputsStopping) {
        return false;
    }
    if (m_txTimeout.latched()) {
        m_txTimeout.refreshLatch();
        publishTxTimeoutLatch(true);
        if (m_txTimeout.latched()) {
            if (request) {
                noteTxTimeoutRefused(*request);
            }
            emitInterlockNotification(
                tr("Transmit is held after a time-out: release every PTT input, then key again."),
                QStringLiteral("tx-timeout-latched"));
            return false;
        }
    }
    // Fail closed: an input whose release the latch could not track is not
    // admitted. CWX is sent text and never holds (below).
    if (request && activity != TxActivity::Cwx && !txTimeoutCanHold(*request)) {
        emitInterlockNotification(
            tr("Transmit refused: too many inputs hold a key. Release one, then key again."),
            QStringLiteral("tx-timeout-holders-full"));
        return false;
    }
    if (request && !m_txCoordinator.acceptsRequest(*request)) {
        return false;
    }
    const unsigned services = static_cast<unsigned>(TxActivity::VoiceKeyer)
        | static_cast<unsigned>(TxActivity::Navtex);
    const bool serviceActivity = static_cast<unsigned>(activity) & services;
    if (m_txCoordinator.hasOtherIntents(m_txOperation, {}, serviceActivity ? 0 : services)) {
        emitInterlockNotification(serviceActivity
                ? tr("The voice keyer or NAVTEX waits for the current transmission to end.")
                : tr("Transmit is held by the voice keyer or a NAVTEX message."),
            QStringLiteral("tx-service"));
        return false;
    }
    // The radio's own report decides too: a Stop releases this client's hold
    // at once, but a playback or NAVTEX transmission the radio still reports
    // keeps every other transmit out (maintainer ruling).
    if (!serviceActivity && (m_dvkModel.status() == VoiceKeyer::Playback
            || m_navtexModel.status() == NavtexStatus::Transmitting)) {
        emitInterlockNotification(tr("Transmit is held: the radio reports the voice keyer or NAVTEX transmitting."),
                                  QStringLiteral("tx-service"));
        return false;
    }
    if (m_daxTxOwnershipIntent) {
        qCWarning(lcProtocol) << "RadioModel: TX refused — a DAX TX stream selection was pending;"
                              << "the selection is interrupted, key again to transmit";
        interruptDaxTxOwnership(tr("Transmit preparation interrupted DAX TX ownership. Waiting for fresh radio status."));
        return false;
    }
    // TUNE/ATU use the backend's shared carrier/PTT latch, unlike compatible
    // MOX holds. Their release cannot preserve another producer's carrier.
    // Keep these combinations within one producer until a backend provides
    // independent ownership; never silently transfer another caller's latch.
    const unsigned carrierActivities = static_cast<unsigned>(TxActivity::Tune)
        | static_cast<unsigned>(TxActivity::Atu);
    if (m_txCoordinator.hasForeignIntents(m_txOperation, request,
            (static_cast<unsigned>(activity) & carrierActivities) ? 0 : carrierActivities)) {
        return false;
    }
    if (activity == TxActivity::Tune || activity == TxActivity::Atu || activity == TxActivity::Cwx) {
        const TxCoordinator::Intent previous = request ? m_txCoordinator.requestIntent(*request)
            : m_localTxIntents.value(activity);
        // These are singleton generators/queues, unlike compatible MOX contributors.
        // Replacing another producer's latch/context would silently transfer
        // ownership; a new caller must submit fresh intent after it finishes.
        if (m_txCoordinator.hasOtherIntents(m_txOperation, previous, static_cast<unsigned>(activity))) {
            return false;
        }
    }
    if (m_txSessionClosing) {
        // Admission stays closed from the teardown until onConnected(), so a
        // connect attempt reaches this refusal too; name the phase it is in.
        if (m_connectAttemptActive) {
            emitInterlockNotification(tr("Transmit is unavailable until the radio has connected."),
                                      QStringLiteral("tx-session-connecting"));
        } else {
            emitInterlockNotification(tr("Transmit is unavailable while the radio disconnects."),
                                      QStringLiteral("tx-session-closing"));
        }
        return false;
    }
    if (!m_backend || !refuseKeyOnTransmitIncapableBackend()
        || !refuseKeyInReceiveOnlyMode()) {
        return false;
    }
    const RadioCapabilities caps = backendCapabilities();
    if ((activity == TxActivity::Cwx && !caps.hasRadioSideCwKeyer && !caps.hostCwKeyingControl)
        || (activity == TxActivity::Atu && !caps.hasTuner)) {
        emitInterlockNotification(tr("This radio does not support the requested transmit operation."),
                                  QStringLiteral("tx-operation-unsupported"));
        return false;
    }
    const QString gate = activity == TxActivity::Tune || activity == TxActivity::Atu
        ? QStringLiteral("tune-start")
        : activity == TxActivity::Mox ? QStringLiteral("xmit")
        : activity == TxActivity::VoiceKeyer ? QStringLiteral("dvk")
        : activity == TxActivity::Navtex ? QStringLiteral("navtex") : QStringLiteral("cw-key");
    if (transmitStartBlockedByInhibit(gate)) {
        return false;
    }
    // After the radio's own refusals, so a pan inhibit is reported ahead of it.
    if (askKeyObjection && transmitStartBlockedByKeyObjection(gate)) {
        return false;
    }
    const bool independent = request && m_txCoordinator.isIndependentRequest(*request);
    if (independent
        && (!(m_backend->independentTxControl().activities & static_cast<unsigned>(activity))
            || (!m_txCoordinator.requestOperation(*request).sameOperation(m_txOperation)
                && !m_backend->independentTxReady()))) {
        return false;
    }
    if (!independent && !m_backendTxProducer.valid()) {
        m_backendTxProducer = registerTxProducer(m_backend.get());
    }
    if (!independent && !m_backendTxProducer.valid()) {
        emitInterlockNotification(tr("Transmit producer capacity is exhausted."),
                                  QStringLiteral("tx-producer-capacity"));
        return false;
    }
    // Captured inputs retain their trusted actor. An unbound/invalid input
    // must never fall back to the desktop compatibility actor.
    const TxCoordinator::Actor actor = request ? m_txCoordinator.requestActor(*request) : m_desktopTxActor;
    const TxCoordinator::Admission admission = m_txCoordinator.acquire(actor, txMonotonicMs());
    if (!admission.accepted()) {
        // One message per reason. Only Recovering is reachable while a single
        // desktop actor exists, but the others become reachable as soon as a
        // per-client actor does, and "cleanup is in progress" would then be a
        // wrong explanation rather than a vague one.
        QString message;
        QString key;
        switch (admission.refusal) {
        case TxCoordinator::Refusal::Recovering:
            message = tr("Transmit cleanup is still in progress.");
            key = QStringLiteral("tx-coordinator-recovering");
            break;
        case TxCoordinator::Refusal::Busy:
            message = tr("Another client is transmitting.");
            key = QStringLiteral("tx-coordinator-busy");
            break;
        case TxCoordinator::Refusal::Denied:
            message = tr("This client is not permitted to transmit.");
            key = QStringLiteral("tx-coordinator-denied");
            break;
        case TxCoordinator::Refusal::InvalidActor:
        case TxCoordinator::Refusal::WrongThread:
        case TxCoordinator::Refusal::None:
            message = tr("Transmit is unavailable.");
            key = QStringLiteral("tx-coordinator-unavailable");
            break;
        }
        // m_txSessionClosing short-circuits above, so a Recovering refusal here
        // is a stop that was never acknowledged rather than a normal disconnect
        // gap. That is a permanent admission latch, so say so instead of
        // leaving it to be diagnosed from a silent refusal. See
        // TxCoordinator::acknowledgeStopped().
        if (admission.refusal == TxCoordinator::Refusal::Recovering) {
            if (m_txCoordinator.hasInFlightDispatches()) {
                qCWarning(lcProtocol) << "RadioModel: TX refused — preceding terminal writer still entered;"
                                     << "retry requires fresh intent after it returns";
            } else {
                qCWarning(lcProtocol)
                    << "RadioModel: TX refused — coordinator stop is unacknowledged;"
                    << "admission stays closed until the session ends";
            }
        }
        emitInterlockNotification(message, key);
        return false;
    }
    if (!m_txOperation.sameOperation(admission.operation)) {
        m_pendingTxDeliveries = 0;
        m_txOperationActivities = 0;
    }
    m_txOperation = admission.operation;
    const TxCoordinator::Intent intent = request
        ? m_txCoordinator.beginRequest(*request, m_txOperation, activity)
        : m_txCoordinator.beginIntent(m_txOperation, m_localTxIntents.value(activity), activity);
    if (!intent.pending()) {
        qCWarning(lcProtocol) << "RadioModel: TX producer intent could not be registered";
        completeLocalTxIfDrained();
        return false;
    }
    if (!request) {
        m_localTxIntents.insert(activity, intent);
    }
    if (activity == TxActivity::Cwx) {
        if (!intent.sameIntent(m_cwxCommandIntent)) {
            m_cwxPendingDeliveries = 0;
            m_cwxHandoffComplete = false;
        }
        m_cwxCommandIntent = intent;
        m_cwxCommandOperation = request ? m_txCoordinator.requestOperation(*request) : m_txOperation;
    }
    m_txOperationActivities |= static_cast<unsigned>(activity);
    m_daxTxSelectionRxObserved = false;
    // Independent backend-generated media belongs to this captured grant's
    // input. The shared desktop producer cannot carry another actor's audio.
    // Keep desktop's compatible multi-contributor context unchanged.
    m_backend->setTransmitContext(independent ? m_txCoordinator.mediaContext(*request)
        : m_txCoordinator.mediaContext(m_backendTxProducer,
            request ? m_txCoordinator.requestOperation(*request) : m_txOperation));
    if (request) {
        // Arm source-specific policing before a backend command or synchronous
        // UI notification can enter a nested event loop. Recheck after it:
        // observers may revoke this authorization or tear down the session.
        m_txCoordinator.notifyProducerAdmission(*request);
        if (!request->valid()) {
            endLocalTxActivity(intent);
            return false;
        }
    }
    const QPointer<RadioModel> self(this);
    emit daxTxOwnershipChanged();
    const bool admitted = self && !m_txSessionClosing && m_backend
        && (request ? request->valid() : intent.permitsDispatch(txMonotonicMs()));
    // CWX is sent text, not a held input: no producer sends an off for it,
    // so it never holds the time-out latch.
    if (admitted && request && activity != TxActivity::Cwx && !noteTxTimeoutHold(*request)) {
        // The table filled while this key-down was admitted (an observer above
        // re-entered): refuse it rather than key an input the latch cannot see.
        endLocalTxActivity(intent);
        return false;
    }
    return admitted;
}

TransmitModel::KeyingRoute RadioModel::producerKeyingRoute(const TxCoordinator::Request& request,
                                                          TxActivity activity, bool& dispatched)
{
    return {
        [this, request, activity](bool on) -> TransmitModel::KeyingPermit {
            if (on) {
                if (!beginTxActivity(activity, &request)) {
                    return {};
                }
                const TxCoordinator::Operation operation = m_txCoordinator.requestOperation(request);
                return [operation] { return operation.permitsDispatch(txMonotonicMs()); };
            }
            const TxCoordinator::Intent bound = m_txCoordinator.requestIntent(request);
            if (!bound.isActivity(activity)) {
                if (!bound.pending()) {
                    (void)m_txCoordinator.closeRequest(request);
                }
                return {};
            }
            const TxCoordinator::Intent intent = m_txCoordinator.closeRequest(request);
            if (!intent.pending()) {
                return {};
            }
            const TxCoordinator::Operation operation = m_txCoordinator.requestOperation(request);
            if (m_txCoordinator.hasOtherIntents(operation, intent, static_cast<unsigned>(activity))) {
                endLocalTxActivity(intent);
                return {};
            }
            return [operation, intent] { return operation.permitsCleanup() && intent.pending(); };
        },
        [this, request, activity, &dispatched](bool on) {
            if (activity == TxActivity::Tune) {
                dispatched = dispatchTuneIntent(on, &request);
            } else {
                dispatched = dispatchAtuIntent(on, &request);
            }
        }};
}

bool RadioModel::requestProducerTune(const TxCoordinator::Request& request, bool on, bool twoTone)
{
    if (QThread::currentThread() != thread()) {
        return false;
    }
    if (!on) {
        noteTxTimeoutRelease(request);
    }
    bool dispatched = false;
    const TransmitModel::KeyingRoute route = producerKeyingRoute(request, TxActivity::Tune, dispatched);
    if (on) {
        m_transmitModel.requestTune(TransmitModel::PttSource::Tune, twoTone, route);
    } else {
        m_transmitModel.stopTune(route);
    }
    const bool accepted = dispatched;
    if (on && !accepted) {
        m_transmitModel.stopTune(route); // retire even a partially admitted request
    }
    return accepted;
}

bool RadioModel::requestProducerAtu(const TxCoordinator::Request& request, bool start)
{
    if (QThread::currentThread() != thread()) {
        return false;
    }
    if (!start) {
        noteTxTimeoutRelease(request);
    }
    bool dispatched = false;
    const TransmitModel::KeyingRoute route = producerKeyingRoute(request, TxActivity::Atu, dispatched);
    m_transmitModel.requestAtu(start, route);
    // A start withdrawn during admission dispatches its cleanup, not a cycle.
    const bool accepted = dispatched && (!start || m_transmitModel.atuAvailable());
    if (start && !accepted) {
        m_transmitModel.requestAtu(false, route);
    }
    return accepted;
}

bool RadioModel::requestProducerAtuBypass(const TxCoordinator::Request& request)
{
    if (QThread::currentThread() != thread()) {
        return false;
    }
    const TxCoordinator::Intent intent = m_txCoordinator.requestIntent(request);
    if (intent.pending()) {
        return requestProducerAtu(request, false);
    }
    // Bypass also configures idle ATU relays; it is not only a stop edge.
    // Preserve that operation without letting it interrupt somebody else's
    // matching cycle. The captured request fences delayed UI input/reconnects.
    if (!request.valid() || m_txSessionClosing || !m_backend
        || m_txCoordinator.hasOtherIntents(m_txOperation, {}, static_cast<unsigned>(TxActivity::Atu))) {
        return false;
    }
    (void)m_txCoordinator.closeRequest(request);
    m_transmitModel.atuBypass();
    return true;
}

bool RadioModel::dispatchTuneIntent(bool on, const TxCoordinator::Request* request)
{
    const quint64 commandEpoch = ++m_tuneCommandEpoch;
    const TxCoordinator::Operation operation = request ? m_txCoordinator.requestOperation(*request) : m_txOperation;
    const TxCoordinator::Intent intent = request ? m_txCoordinator.requestIntent(*request)
        : m_localTxIntents.value(TxActivity::Tune);
    // The backend has one tune latch, so an unrouted stop ends every producer's
    // carrier. Their contributions end with it, or a stale one refuses every
    // later TUNE start until its own producer stops again. Captured before the
    // dispatch, so a local TUNE re-engaged from the stop's own edge (a fresh
    // intent) survives; a producer re-engaging that way reuses its bound one.
    std::vector<TxCoordinator::Intent> latchHolders;
    if (!on && !request) {
        (void)m_txCoordinator.requestIntentEnd(intent);
        latchHolders = m_txCoordinator.intents(m_txOperation, static_cast<unsigned>(TxActivity::Tune));
    }
    if (on) {
        armInterlockNotification(m_transmitModel.activePttSource());
    }
    const TxCoordinator::Operation cleanup = request ? operation : m_txCoordinator.cleanupFence();
    bool releaseQueued = false;
    bool dispatched = false;
    if (m_backend && commandEpoch == m_tuneCommandEpoch
        && (on ? operation.permitsDispatch(txMonotonicMs()) : cleanup.permitsCleanup())) {
        if (on) {
            const quint64 previousInhibitEpoch = m_tuneInhibitEpoch;
            if (!applyTuneInhibit()) {
                return false;
            }
            if (!m_backend || commandEpoch != m_tuneCommandEpoch
                || !operation.permitsDispatch(txMonotonicMs())) {
                if (m_tuneInhibitEpoch == previousInhibitEpoch + 1) {
                    restoreTuneInhibit();
                }
                return false;
            }
        }
        const QPointer<RadioModel> receiver(this);
        const auto finished = request && !on ? std::function<void()>([receiver, intent] {
            if (receiver) {
                receiver->endLocalTxActivity(intent);
            }
        }) : std::function<void()>{};
        releaseQueued = request && !on;
        dispatched = true;
        m_backend->setTune(on, m_transmitModel.tunePower(), on ? operation : cleanup,
                           trackTxQueue(operation, finished));
        if (commandEpoch == m_tuneCommandEpoch) {
            publishCommandedBackendTransmitEdge(on);
        }
    }
    if (!on && !releaseQueued) {
        endLocalTxActivity(intent);
    }
    for (const TxCoordinator::Intent& holder : latchHolders) {
        (void)m_txCoordinator.requestIntentEnd(holder);
        endLocalTxActivity(holder);
    }
    return dispatched;
}

bool RadioModel::dispatchAtuIntent(bool start, const TxCoordinator::Request* request)
{
    const quint64 commandEpoch = ++m_atuCommandEpoch;
    const TxCoordinator::Operation operation = request ? m_txCoordinator.requestOperation(*request) : m_txOperation;
    const TxCoordinator::Intent intent = request ? m_txCoordinator.requestIntent(*request)
        : m_localTxIntents.value(TxActivity::Atu);
    if (!start && !request) {
        (void)m_txCoordinator.requestIntentEnd(intent);
    }
    if (start) {
        m_transmitModel.noteActivePttSource(TransmitModel::PttSource::Atu);
        armInterlockNotification(TransmitModel::PttSource::Atu);
    }
    const TxCoordinator::Operation cleanup = request ? operation : m_txCoordinator.cleanupFence();
    bool releaseQueued = false;
    bool dispatched = false;
    if (m_backend && commandEpoch == m_atuCommandEpoch
        && (start ? operation.permitsDispatch(txMonotonicMs()) : cleanup.permitsCleanup())) {
        if (start) {
            const quint64 previousInhibitEpoch = m_tuneInhibitEpoch;
            if (!applyTuneInhibit()) {
                return false;
            }
            if (!m_backend || commandEpoch != m_atuCommandEpoch
                || !operation.permitsDispatch(txMonotonicMs())) {
                if (m_tuneInhibitEpoch == previousInhibitEpoch + 1) {
                    restoreTuneInhibit();
                }
                return false;
            }
        }
        m_atuCommandIntent = intent;
        const QPointer<RadioModel> receiver(this);
        const auto finished = request && !start ? std::function<void()>([receiver, intent] {
            if (receiver) {
                receiver->endLocalTxActivity(intent);
            }
        }) : std::function<void()>{};
        releaseQueued = request && !start;
        dispatched = true;
        m_backend->setAtu(start, start ? operation : cleanup, trackTxQueue(operation, finished));
    }
    if (!start && !releaseQueued) {
        endLocalTxActivity(intent);
    }
    return dispatched;
}

void RadioModel::endLocalTxActivity(const TxCoordinator::Intent& intent)
{
    if (m_txCoordinator.endIntent(intent)) {
        completeLocalTxIfDrained();
        emit daxTxOwnershipChanged();
    }
}

unsigned RadioModel::activeTxActivities() const
{
    // Include older draining contributions, not just the current compatibility
    // slot. A later completed edge cannot hide an earlier pending CW tail.
    return m_txCoordinator.activeActivities(m_txOperation);
}

// A TUNE this client admitted and still holds. A tune state decoded off the
// radio alone is not one, so live tune power neither reaches it nor hides its
// drop notice.
bool RadioModel::tuneCarrierLive() const
{
    return m_transmitModel.isTuning()
        && (activeTxActivities() & static_cast<unsigned>(TxActivity::Tune)) != 0;
}

bool RadioModel::hasOtherPttHolds(const TxCoordinator::Operation& operation,
                                 const TxCoordinator::Intent& excluded) const
{
    // A CW element/text queue is not a manual PTT hold. In semi-break-in it
    // only rides an already-held carrier; swallowing MOX-off for it strands
    // the latch because its eventual key-up/text drain never releases MOX.
    constexpr unsigned kHoldingActivities = static_cast<unsigned>(TxActivity::Mox)
        | static_cast<unsigned>(TxActivity::CwPtt) | static_cast<unsigned>(TxActivity::Tune)
        | static_cast<unsigned>(TxActivity::Atu);
    return m_txCoordinator.hasOtherIntents(operation, excluded, kHoldingActivities, false);
}

void RadioModel::completeLocalTxIfDrained()
{
    if (!m_txCoordinator.hasIntents(m_txOperation) && m_pendingTxDeliveries == 0) {
        // Existing desktop sequencers explicitly end their local intent. This
        // fences pending work; it is NOT a claim that the radio is observed RX.
        // The coordinator retains this actor's ownership until qualified
        // acknowledgment; only this same compatibility actor can reengage.
        (void)m_txCoordinator.finishLocalIntent(m_txOperation);
        requestIndependentTxStop(m_txOperation);
    }
}

TxGrantManager* RadioModel::independentTxGrants()
{
    if (QThread::currentThread() != thread()) {
        return nullptr;
    }
    if (!m_independentTxGrants) {
        m_independentTxGrants = std::make_unique<TxGrantManager>(m_txCoordinator, [this] {
            return !m_txSessionClosing && m_backend
                ? m_backend->independentTxControl().activities : 0U;
        });
    }
    return m_independentTxGrants.get();
}

bool RadioModel::independentTxReady() const
{
    return QThread::currentThread() == thread() && !m_txSessionClosing && m_backend
        && m_backend->independentTxReady() && !m_txCoordinator.hasOwnership()
        && !m_txCoordinator.recovering();
}

void RadioModel::requestIndependentTxStop(const TxCoordinator::Operation& operation)
{
    if (!operation.independent() || !operation.permitsCleanup()
        || !m_backend || m_independentStop.matchesOperation(operation)) {
        return;
    }
    m_independentStop = m_txCoordinator.requestStopConfirmation(operation);
    if (m_independentStop.valid()) {
        m_backend->stopIndependentTx(operation, m_independentStop);
    }
}

void RadioModel::acknowledgeIndependentTxStop(const TxStopEvidence& evidence)
{
    if (!evidence.valid() || !evidence.request.sameRequest(m_independentStop)) {
        return;
    }
    if (!m_txCoordinator.confirmStopped(evidence.request)
        && m_txCoordinator.hasInFlightDispatches()) {
        // Recheck the ORIGINAL revocable certificate after the writer exits.
        // The timer never supplies radio evidence or a replacement token.
        QTimer::singleShot(1, this, [this, evidence] { acknowledgeIndependentTxStop(evidence); });
    }
}

void RadioModel::acknowledgeTxTransportTeardown(const TxCoordinator::Operation& operation)
{
    // The lifecycle caller has already established transport teardown. An
    // entered writer can still be returning through a reentrant disconnect;
    // retry that bookkeeping only. This timer never supplies radio-stop proof.
    if (!operation.sameOperation(m_txOperation)
        || m_txCoordinator.acknowledgeStopped(operation)
        || !m_txCoordinator.recovering() || !m_txCoordinator.hasInFlightDispatches()) {
        return;
    }
    QTimer::singleShot(10, this, [this, operation] {
        acknowledgeTxTransportTeardown(operation);
    });
}

std::function<void()> RadioModel::trackTxDelivery(const TxCoordinator::Operation& operation)
{
    const bool tracked = operation.sameOperation(m_txOperation) && operation.permitsCleanup();
    if (tracked) {
        m_daxTxSelectionRxObserved = false;
        ++m_pendingTxDeliveries;
    }
    return [this, operation, tracked] {
        if (!tracked || !m_txOperation.sameOperation(operation)
            || m_pendingTxDeliveries == 0) {
            return;
        }
        --m_pendingTxDeliveries;
        completeLocalTxIfDrained();
        emit daxTxOwnershipChanged();
    };
}

TxCoordinator::Completion RadioModel::trackTxQueue(const TxCoordinator::Operation& operation,
                                                   std::function<void()> finished)
{
    const QPointer<RadioModel> receiver(this);
    const auto consumed = trackTxDelivery(operation);
    const auto finish = [receiver, consumed, finished] {
        if (receiver) {
            consumed();
        }
        if (receiver && finished) {
            finished();
        }
    };
    return TxCoordinator::Completion([receiver, finish] {
        if (receiver) {
            if (QThread::currentThread() == receiver->thread()) {
                finish();
            } else {
                QMetaObject::invokeMethod(receiver, finish, Qt::QueuedConnection);
            }
        }
    });
}

bool RadioModel::sendTxKeyingCommand(const QString& command, const TxCoordinator::Command& fence)
{
    const TxCoordinator::Operation operation = fence.operation;
    const auto completed = [completion = fence.completion] { completion.finish(); };
    if (fence.keying) {
        if (!sendTxTcpCommand(command, operation, true, completed)) {
            completed();
            return false;
        }
        return true;
    }
    // Retain a short, normally released operation until its queued key-up is
    // consumed, just as NetCW does. Otherwise completion cancels an earlier
    // key-on before the transport has had a chance to consume either edge.
    // This is local queue completion, not qualified radio-idle evidence.
    if (!sendTxTcpCommand(command, operation, false, completed)) {
        completed();
        return false;
    }
    return true;
}

bool RadioModel::requestProducerCwx(const TxCoordinator::Request& request, const QString& text,
                                   std::function<void()> onAdmitted, int macroIndex, bool live)
{
    const QPointer<RadioModel> self(this);
    if (QThread::currentThread() != thread()) {
        return false;
    }
    if (macroIndex < 0 || macroIndex > 12 || (macroIndex == 0 && !cwTextValidationError(text).isEmpty())) {
        if (!m_hostCwText || !m_hostCwText->hasTextFor(request)) {
            abortProducerCwx(request);
        }
        return false;
    }
    const bool host = hostCwTextRoute();
    if (host) {
        bool held = false;
        if (!admitHostCwx(request, text, onAdmitted, macroIndex, live, held)) {
            // A refused append leaves the sender's playing and waiting host text alone.
            if (!m_hostCwText || !m_hostCwText->hasTextFor(request)) {
                abortProducerCwx(request);
            }
            return false;
        }
        if (held) {
            return true;
        }
    }
    bool admitted = false;
    const CwxModel::TransmissionRoute route{
        [self, request, host, &admitted, &onAdmitted]() -> CwxModel::TransmissionPermit {
            if (!self || !self->beginTxActivity(TxActivity::Cwx, &request) || !self) {
                return {};
            }
            admitted = true;
            if (host) {
                // One speed source for host keying, adopted once the send is admitted.
                self->m_cwxModel.adoptSpeed(self->m_transmitModel.cwSpeed());
            }
            const TxCoordinator::Operation operation = self->m_txCoordinator.requestOperation(request);
            if (onAdmitted) {
                onAdmitted();
            }
            return [operation] { return operation.permitsDispatch(txMonotonicMs()); };
        },
        [self, request](const QString& value, int wpm) {
            if (!self) {
                return false;
            }
            return self->hostCwTextRoute() ? self->dispatchHostCwText(request, value, wpm)
                : self->dispatchCwxText(value, self->m_txCoordinator.requestOperation(request));
        },
        [self, request](const CwTextRequest& command, int epoch, int nChars, CwxModel::TransmissionPermit current) {
            if (!self) {
                return ReceiveDispatch::Unsupported;
            }
            return self->hostCwTextRoute() ? self->dispatchHostCwxRequest(command)
                : self->dispatchCwxRequest(command, self->m_txCoordinator.requestOperation(request),
                                           epoch, nChars, std::move(current));
        },
        [self, request](int epoch, bool untrackedMacro) {
            if (self) { self->finishCwxDispatch(epoch, untrackedMacro, self->m_txCoordinator.requestIntent(request)); }
        }};
    if (macroIndex != 0) {
        m_cwxModel.sendMacro(macroIndex, route);
    } else if (live) {
        m_cwxModel.sendChar(text, route);
    } else {
        m_cwxModel.send(text, route);
    }
    // A refused append leaves the sender's playing host text alone.
    const bool hostPlaying = self && host && m_hostCwText && m_hostCwText->playingFor(request);
    if (self && !admitted && !hostPlaying) {
        abortProducerCwx(request);
    }
    return admitted && (!host || hostPlaying);
}

bool RadioModel::hostCwTextRoute() const
{
    return m_hostCwText && hasHostCwTextKeyer();
}

bool RadioModel::cwKeyHeld() const
{
    return m_cwKeyActive || m_cwPaddleHeld
        || std::any_of(m_producerCwPaddleInputs.begin(), m_producerCwPaddleInputs.end(),
                       [](const TxCoordinator::Request& input) { return input.valid(); });
}

bool RadioModel::admitHostCwx(const TxCoordinator::Request& request, const QString& text,
                              const std::function<void()>& onAdmitted, int macroIndex, bool live,
                              bool& held)
{
    if (!m_txCoordinator.acceptsRequest(request)) {
        return false;
    }
    // Every refusal below names its reason, as the radio-side path's admission does.
    const SliceModel* tx = txSlice();
    const auto refuse = [this, tx](const QString& message, const char* reason) {
        emitInterlockNotification(message, QStringLiteral("cw-text-refused:%1").arg(QLatin1String(reason)),
                                  tx ? tx->panId() : QString());
        return false;
    };
    switch (hostCwTextRefusal()) {
    case HostCwTextRefusal::None:
        break;
    case HostCwTextRefusal::Unsupported:
        return refuse(tr("CW text refused: this radio cannot key CW text."), "unsupported");
    case HostCwTextRefusal::NoTxSlice:
        return refuse(tr("CW text refused: no slice is set to transmit."), "no-tx-slice");
    case HostCwTextRefusal::NotCwMode:
        return refuse(tr("CW text refused: the transmit slice is not in a CW mode."), "not-cw");
    case HostCwTextRefusal::ReceiveOnlyMode:
        return refuse(tr("CW text refused: the transmit slice's mode is receive-only."), "receive-only");
    case HostCwTextRefusal::Inhibited:
        return refuse(tr("CW text refused: transmit is inhibited on the transmit slice."), "inhibited");
    case HostCwTextRefusal::Tuning:
        return refuse(tr("CW text refused: TUNE is active."), "tune");
    case HostCwTextRefusal::BreakInOffUnkeyed:
        return refuse(tr("CW text refused: break-in is off and transmit is not keyed."), "break-in-off");
    }
    // A CW key or paddle already held, local or remote, keeps the key: text
    // started now would key under it (or between paddle elements), and the
    // key's release would cut the text's element. Host text's own elements
    // also hold the key, so a playing run is left out.
    if (cwKeyHeld() && !m_hostCwText->playing()) {
        return refuse(tr("CW text refused: a CW key is held down."), "key-held");
    }
    // No radio holds a macro here: only the local copy can be keyed, and the
    // bounds apply to it once {CALL} is resolved, as it will be keyed.
    QString message = text;
    if (macroIndex != 0) {
        QString reason;
        const std::optional<QString> resolved = m_cwxModel.resolvePlaceholders(m_cwxModel.macro(macroIndex - 1),
                                                                                &reason);
        if (!resolved) {
            return refuse(reason, "placeholder");
        }
        message = *resolved;
    }
    if (!m_hostCwText->admitsLength(static_cast<int>(message.size()))) {
        return refuse(tr("CW text refused: a message holds at most %1 characters, and at most %2 may wait.")
                          .arg(HostCwTextEngine::kMaxMessageChars).arg(HostCwTextEngine::kMaxPendingChars),
                      "length");
    }
    // Text the host keyer cannot key whole would be keyed in part, or not at all.
    if (!CwxLocalKeyer::canEncode(message)) {
        return refuse(tr("CW text refused: it holds a character the CW keyer cannot send."), "encode");
    }
    // Another sender's run plays: this message waits behind it (one CW stream).
    if (m_hostCwText->playing() && !m_hostCwText->playingFor(request)) {
        if (!m_hostCwText->hold({request, message, macroIndex, live})) {
            return refuse(tr("CW text refused: the CW text engine is closed."), "closed");
        }
        held = true;
        if (onAdmitted) {
            onAdmitted();
        }
    }
    return true;
}

ReceiveDispatch RadioModel::dispatchHostCwxRequest(const CwTextRequest& request)
{
    // The engine keys the text segments at their own WPM; no radio holds a macro.
    if (request.field == CwTextRequest::Field::Speed) {
        if (request.origin == CwTextRequest::Origin::Operator) {
            m_transmitModel.setCwSpeed(request.value);
        }
        return ReceiveDispatch::LocalOnly;
    }
    return request.field == CwTextRequest::Field::Send ? ReceiveDispatch::LocalOnly : ReceiveDispatch::Unsupported;
}

bool RadioModel::dispatchHostCwText(const TxCoordinator::Request& request, const QString& text, int wpm)
{
    const TxCoordinator::Operation operation = m_txCoordinator.requestOperation(request);
    if (!m_hostCwText || !operation.permitsDispatch(txMonotonicMs())) {
        return false;
    }
    // The sender's Cwx intent lasts until the engine has keyed this segment.
    if (m_hostCwText->play(request, text, wpm, trackCwxQueue(operation))) {
        return true;
    }
    // Only this sender ends: its closed request stops CwxModel from clearing
    // the buffer, which would drop every other sender's waiting text.
    abortProducerCwx(request);
    return false;
}

void RadioModel::startWaitingHostCwx()
{
    const QPointer<RadioModel> self(this);
    while (self && m_hostCwText) {
        std::optional<HostCwTextEngine::Waiting> next = m_hostCwText->takeNext();
        if (!next) {
            return;
        }
        if (!requestProducerCwx(next->sender, next->macroIndex != 0 ? QString() : next->text, {},
                                next->macroIndex, next->live) && self) {
            emitInterlockNotification(tr("Queued CW text was not sent: transmit refused it when its turn came."),
                                      QStringLiteral("cw-text-queued-dropped"));
            // The sender's other waiting text goes too; with nothing playing
            // for it either, its request is closed.
            if (m_hostCwText) {
                m_hostCwText->dropWaiting(next->sender);
                if (!m_hostCwText->playingFor(next->sender)) {
                    abortProducerCwx(next->sender);
                }
            }
        }
    }
}

void RadioModel::hostCwTextEnded(const TxCoordinator::Request& sender, bool refused)
{
    if (refused) {
        abortProducerCwx(sender);   // ends its Cwx intent and drops its waiting text
    }
    startWaitingHostCwx();
}

void RadioModel::createHostCwTextEngine()
{
    const RadioCapabilities caps = m_backend ? backendCapabilities() : RadioCapabilities{};
    if (m_hostCwText || !m_backend || !caps.hostCwKeyingControl || caps.hasRadioSideCwKeyer) {
        return;
    }
    const QPointer<RadioModel> self(this);
    m_hostCwText = std::make_unique<HostCwTextEngine>(this,
        [self](const TxCoordinator::Request& sender, bool refused) {
            if (self) {
                self->hostCwTextEnded(sender, refused);
            }
        });
}

void RadioModel::abortProducerCwx(const TxCoordinator::Request& request)
{
    if (QThread::currentThread() != thread()) {
        return;
    }
    // Host text: only this sender's run and waiting text stop, key up now.
    const bool hostPlaying = m_hostCwText && m_hostCwText->playingFor(request);
    if (m_hostCwText) {
        m_hostCwText->stop(request);
    }
    const auto next = qScopeGuard([this, hostPlaying] {
        if (hostPlaying) {
            startWaitingHostCwx();
        }
    });
    const TxCoordinator::Intent intent = m_txCoordinator.requestIntent(request);
    if (!intent.isActivity(TxActivity::Cwx)) {
        if (!intent.pending()) {
            (void)m_txCoordinator.closeRequest(request);
        }
        return;
    }
    const TxCoordinator::Intent closed = m_txCoordinator.closeRequest(request);
    const TxCoordinator::Operation operation = m_txCoordinator.requestOperation(request);
    if (closed.isActivity(TxActivity::Cwx) && operation.permitsCleanup()
        && intent.sameIntent(m_cwxCommandIntent)) {
        if (m_hostCwText) {
            endLocalTxActivity(intent);
            return;
        }
        // This includes the unacknowledged radio-side text tail after local
        // handoff, but never a replacement producer's queue, even in one over.
        m_cwxModel.clearBuffer();
    }
}

ReceiveDispatch RadioModel::dispatchCwxRequest(const CwTextRequest& request,
    TxCoordinator::Operation operation, int epoch, int nChars, RadioControlFence current)
{
    if (QThread::currentThread() != thread() || !request.valid() || (current && !current())) {
        return ReceiveDispatch::Unsupported;
    }
    if (request.configuration() && !isConnected()) { return ReceiveDispatch::LocalOnly; }
    if (!m_backend || !isConnected() || m_txSessionClosing || m_expiringPendingCallbacks) {
        return ReceiveDispatch::Unsupported;
    }
    const RadioCapabilities caps = backendCapabilities();
    if (!caps.cwTextControl) {
        // Existing native text adapters dispatch once through TextSender. Their
        // speed setting retains TransmitModel's typed hold-delay policy.
        if (!caps.hasRadioSideCwKeyer) { return ReceiveDispatch::Unsupported; }
        if (request.field == CwTextRequest::Field::Speed) {
            if (request.origin == CwTextRequest::Origin::Operator) {
                m_transmitModel.setCwSpeed(request.value);
            }
            return ReceiveDispatch::LocalOnly;
        }
        if (request.keys() || request.field == CwTextRequest::Field::Clear
            || request.field == CwTextRequest::Field::Erase) { return ReceiveDispatch::LocalOnly; }
        return ReceiveDispatch::Unsupported;
    }
    if (!caps.cwTextControl->supports(request.field)) { return ReceiveDispatch::Unsupported; }
    if (request.configuration()) {
        return m_backend->requestCwTextControl(request, {{}, false}, current);
    }
    const bool batchSpeed = request.origin != CwTextRequest::Origin::Operator;
    if (request.keys() && !operation.permitsDispatch(txMonotonicMs())) { return ReceiveDispatch::Unsupported; }
    if (request.origin == CwTextRequest::Origin::Transient) {
        if (!operation.permitsDispatch(txMonotonicMs())) { return ReceiveDispatch::Unsupported; }
        current = [operation, captured = std::move(current)] {
            return operation.permitsDispatch(txMonotonicMs()) && (!captured || captured());
        };
    }
    if (!request.keys() && !operation.permitsCleanup()) { return ReceiveDispatch::Unsupported; }
    const QPointer<RadioModel> self(this);
    const RadioControlSession session = controlSession();
    CwTextCompletion reply;
    if (request.keys()) {
        m_cwxActive = true;
        if (nChars >= 0
            && caps.cwTextControl->completion == CwTextControl::Completion::RadioCharacterIndex) {
            m_cwxDrainArmed = true;
            reply = [self, session, operation, epoch, nChars](const CwTextReply& result) {
                if (self && self->controlSessionCurrent(session)
                    && operation.permitsDispatch(txMonotonicMs())) {
                    self->m_cwxModel.handleSendReply(result, epoch, nChars);
                }
            };
        }
    } else if (request.field == CwTextRequest::Field::Clear) {
        m_cwxActive = false;
        m_cwxDrainArmed = false;
    }
    const bool counted = request.keys() || batchSpeed || request.field == CwTextRequest::Field::Clear
        || request.field == CwTextRequest::Field::Erase;
    const TxCoordinator::Completion consumed = counted ? trackCwxQueue(operation) : TxCoordinator::Completion{};
    const TxCoordinator::Command command{operation, request.keys(), consumed};
    const ReceiveDispatch result = m_backend->requestCwTextControl(request, command, current, reply);
    if (result == ReceiveDispatch::Unsupported) { consumed.finish(); }
    if (!self) { return ReceiveDispatch::Unsupported; }
    return result;
}

bool RadioModel::dispatchCwxText(const QString& text, TxCoordinator::Operation original)
{
    if (backendCapabilities().cwTextControl) {
        return true;
    }
    if (!m_backend || !backendCapabilities().hasRadioSideCwKeyer
        || !original.permitsDispatch(txMonotonicMs())) {
        return false;
    }
    const TxCoordinator::Operation operation = original.withKeyingPermit(m_cwxModel.queuedTransmissionPermit());
    const QString rejection = m_backend->sendCwText(text, operation, trackCwxQueue(operation));
    if (!rejection.isEmpty()) {
        emit radioMessageReceived(tr("CW text not sent: %1").arg(rejection), MessageSeverity::Warning);
    }
    return rejection.isEmpty();
}

void RadioModel::finishCwxDispatch(int epoch, bool untrackedMacro, TxCoordinator::Intent intent)
{
    if (epoch != m_cwxModel.drainEpoch() || !intent.sameIntent(m_cwxCommandIntent)) {
        return;
    }
    const auto control = backendCapabilities().cwTextControl;
    if (!control || control->completion == CwTextControl::Completion::LocalHandoff || untrackedMacro) {
        // CI-V and unknown-length Flex macros have no qualified drain index.
        // This is local queue handoff, never proof of radio-idle or recovery.
        if (untrackedMacro && m_cwxDrainArmed) {
            m_cwxDrainArmed = false;
            m_cwxModel.abandonDrainWatch();
        }
        m_cwxHandoffComplete = true;
        if (m_cwxPendingDeliveries == 0) {
            endLocalTxActivity(intent);
        }
    }
}

TxCoordinator::Completion RadioModel::trackCwxQueue(const TxCoordinator::Operation& operation)
{
    const TxCoordinator::Intent intent = m_cwxCommandIntent;
    ++m_cwxPendingDeliveries;
    const QPointer<RadioModel> receiver(this);
    return trackTxQueue(operation, [receiver, intent] {
        if (!receiver || !intent.sameIntent(receiver->m_cwxCommandIntent)
            || receiver->m_cwxPendingDeliveries == 0) {
            return;
        }
        --receiver->m_cwxPendingDeliveries;
        if (receiver->m_cwxHandoffComplete && receiver->m_cwxPendingDeliveries == 0) {
            receiver->endLocalTxActivity(intent);
        }
    });
}

void RadioModel::stopTxOperation(const TxCoordinator::Operation& operation,
                               TxCoordinator::StopReason reason)
{
    Q_UNUSED(reason);
    // TxCoordinator has already invalidated the keying fence and entered
    // recovery. All cleanup below is key-up/bypass/abort; never re-admit it.
    if (operation.independent() && !m_txOperation.sameOperation(operation)) {
        // Acquiring a lease does not key or adopt it into the model. Canceling
        // such a lease still requires an exact, transport-local no-write proof.
        m_txOperation = operation;
        m_txOperationActivities = 0;
        m_pendingTxDeliveries = 0;
    }
    requestTransmitStop(operation);
    m_localTxIntents.clear();
    m_txOperation = operation;
    // No acknowledgment here: queued stop commands are not stopped-radio
    // evidence. The lifecycle caller acknowledges only after transport loss.
}

void RadioModel::requestTransmitStop(const TxCoordinator::Operation& operation)
{
    if (QThread::currentThread() != thread()) {
        return;
    }
    const QPointer<RadioModel> radio(this);
    const auto current = [radio, operation] {
        return radio && operation.permitsCleanup()
            && operation.sameOperation(radio->m_txOperation);
    };
    if (!current()) {
        return;
    }
    const unsigned activities = activeTxActivities() | m_txOperationActivities;
    const bool hadCwx = m_txOperationActivities & static_cast<unsigned>(TxActivity::Cwx);
    // Close every original input before notifications or terminal writers can
    // reenter. This ends local intent, not ownership/recovery or radio state.
    const bool wasStopping = m_txInputsStopping;
    m_txInputsStopping = true;
    const auto stopping = qScopeGuard([radio, wasStopping] {
        if (radio) {
            radio->m_txInputsStopping = wasStopping;
        }
    });
    m_txCoordinator.finishLocalIntents(operation);
    m_transmitModel.cancelPttRelease();
    if (current() && hadCwx) {
        m_cwxModel.clearBuffer();
    }
    if (current() && (activities & static_cast<unsigned>(TxActivity::CwKey))) {
        sendCwKey(false);
    }
    if (current() && (activities & static_cast<unsigned>(TxActivity::CwPtt))) {
        sendCwPtt(false);
    }
    if (current() && (activities & static_cast<unsigned>(TxActivity::Tune))) {
        m_transmitModel.stopTune();
    }
    if (current() && (activities & static_cast<unsigned>(TxActivity::Atu))) {
        m_transmitModel.atuBypass();
    }
    if (current()) {
        // Also close a reported tail after normal local handoff. Do not touch
        // ATU relay configuration unless this operation actually requested ATU.
        m_transmitModel.setMox(false);
    }
    if (current()) {
        requestIndependentTxStop(operation);
    }
}

void RadioModel::cancelLocalTransmit()
{
    if (QThread::currentThread() != thread()) {
        return;
    }
    // This is explicit operator Stop, unlike automatic operation retirement.
    // DVK stop is global; automatic cleanup cannot prove another client's
    // replacement has not acquired that radio operation.
    const QPointer<RadioModel> alive(this);
    if (m_dvkModel.status() == VoiceKeyer::Playback
        || m_dvkModel.pendingOperation() == VoiceKeyer::Playback) {
        m_dvkModel.playbackStop(m_dvkModel.activeId());
    }
    if (!alive) { return; }
    // Stop also ends the services' hold on the transmitter, so a NAVTEX
    // submission or voice playback whose answer never came cannot lock out
    // MOX, TUNE or the next submission until reconnect.
    if (m_dvkTxRequest) {
        m_dvkTxRequest = 0;
        endLocalTxActivity(std::exchange(m_dvkTxIntent, {}));
    }
    if (m_navtexTxRequest) {
        m_navtexTxRequest = 0;
        endLocalTxActivity(std::exchange(m_navtexTxIntent, {}));
    }
    if (!alive) { return; }
    m_txCoordinator.discardCapturedRequests();
    m_cwInputSession.fetch_add(1, std::memory_order_release);
    m_cwInputNotBefore = std::chrono::steady_clock::now();
    if (m_txOperation.permitsCleanup()) {
        requestTransmitStop(m_txOperation);
    } else {
        // The operator can also release a reported/manual radio TX for which
        // this client never admitted a local operation. Stop-only, as before.
        const QPointer<RadioModel> radio(this);
        const bool wasStopping = m_txInputsStopping;
        m_txInputsStopping = true;
        const auto stopping = qScopeGuard([radio, wasStopping] {
            if (radio) {
                radio->m_txInputsStopping = wasStopping;
            }
        });
        sendCwKey(false);
        if (radio) {
            radio->sendCwPtt(false);
        }
        if (radio) {
            radio->m_transmitModel.stopTune();
        }
        if (radio) {
            radio->setTransmit(false);
        }
    }
}

void RadioModel::emergencyTransmitStop()
{
    if (QThread::currentThread() != thread()) {
        return;
    }
    m_txCoordinator.emergencyStop();
    cancelLocalTransmit();
}

void RadioModel::addKeyObjection(QObject* owner, KeyObjection objection)
{
    if (QThread::currentThread() != thread() || owner == nullptr || !objection) {
        return;
    }
    m_keyObjections.push_back({QPointer<QObject>(owner), std::move(objection)});
}

QString RadioModel::keyObjectionMessage() const
{
    // A request while this client already transmits is not a key-on: refusing
    // it would roll back the state of a transmission on the air.
    if (activeTxActivities() != 0) {
        return QString();
    }
    return firstKeyObjection();
}

QString RadioModel::firstKeyObjection() const
{
    for (const KeyObjectionEntry& entry : m_keyObjections) {
        if (entry.owner.isNull()) {
            continue;
        }
        const QString message = entry.objection().trimmed();
        if (!message.isEmpty()) {
            return message;
        }
    }
    return QString();
}

void RadioModel::emitKeyObjectionNotice(const QString& message, const QString& gate)
{
    // Keyed by gate, never by the sentence, so a changing sentence does not
    // re-fire the notice inside its de-duplication window.
    const QString panId = txSlice() ? txSlice()->panId() : QString();
    emitInterlockNotification(message, QStringLiteral("key-objection:%1:%2").arg(panId, gate), panId);
}

bool RadioModel::transmitStartBlockedByKeyObjection(const QString& gate)
{
    const QString message = keyObjectionMessage();
    if (message.isEmpty()) {
        return false;
    }
    emitKeyObjectionNotice(message, gate);
    m_transmitModel.setTransmitting(false);
    if (m_txAudioGate) {
        m_txAudioGate = false;
        emit txAudioGateChanged(false);
    }
    return true;
}

void RadioModel::resetTxOperations()
{
    // Host CW text keys up first, while the coordinator can still deliver it.
    if (const std::unique_ptr<HostCwTextEngine> host = std::move(m_hostCwText)) {
        host->close();
    }
    // Even an idle coordinator must refuse new intent while the session dies.
    // Close admission BEFORE cancellation/reply/model notifications can reenter
    // us; operation recovery alone only covers a previously active operation.
    m_txSessionClosing = true;
    {
        const bool latched = m_txTimeout.latched();
        m_txTimeout.reset();
        m_txTimeoutRefusedRequests.clear();
        if (m_txTimeoutTimer) {
            m_txTimeoutTimer->stop();
        }
        publishTxTimeoutLatch(latched);
    }
    m_transmitServiceSession->store(false, std::memory_order_release);
    m_transmitServiceSession = std::make_shared<std::atomic_bool>(true);
    m_dvkTxRequest = 0;
    m_navtexTxRequest = 0;
    m_dvkTxIntent = {};
    m_navtexTxIntent = {};
    m_dvkModel.reset();
    m_navtexModel.reset();
    m_cwxSessionValid->store(false, std::memory_order_release);
    m_cwxSessionValid = std::make_shared<std::atomic<bool>>(true);
    if (m_independentTxGrants) {
        m_independentTxGrants->invalidateRadio();
    }
    emit transmitSessionInvalidated();
    m_independentStop = {};
    m_pendingTxDeliveries = 0;
    m_cwInputSession.fetch_add(1, std::memory_order_release);
    m_cwInputNotBefore = std::chrono::steady_clock::now();
    m_transmitModel.cancelPttRelease();
    m_txCoordinator.reset();
    m_cwxModel.resetDrainWatch();
    m_localTxIntents.clear();
    m_atuCommandIntent = {};
    m_cwxCommandIntent = {};
    m_cwxCommandOperation = {};
    m_cwxPendingDeliveries = 0;
    m_cwxHandoffComplete = false;
}

void RadioModel::queueCwKeyEdge(bool down, const QString& source, quint64 traceId,
                              quint64 sourceMs, std::chrono::steady_clock::time_point scheduledAt)
{
    const quint64 session = m_cwInputSession.load(std::memory_order_acquire);
    QMetaObject::invokeMethod(this, [this, session, down, source, traceId, sourceMs, scheduledAt] {
        if (session != m_cwInputSession.load(std::memory_order_acquire)
            || m_txSessionClosing || scheduledAt < m_cwInputNotBefore) {
            return;
        }
        sendCwKeyEdge(down, source, traceId, sourceMs, scheduledAt);
    }, Qt::QueuedConnection);
}

void RadioModel::queueProducerCwKeyEdge(const TxCoordinator::Request& request, bool down,
                                       const QString& source, quint64 traceId, quint64 sourceMs,
                                       std::chrono::steady_clock::time_point scheduledAt,
                                       bool hostTextGates, std::function<void()> onDownRefused)
{
    QMetaObject::invokeMethod(this, [this, request, down, source, traceId, sourceMs, scheduledAt,
                                     hostTextGates, onDownRefused = std::move(onDownRefused)] {
        const auto refused = [down, &onDownRefused] {
            if (down && onDownRefused) {
                onDownRefused();
            }
        };
        if (m_txSessionClosing || scheduledAt < m_cwInputNotBefore) {
            refused();
            return;
        }
        if (down && hostTextGates) {
            SliceModel* tx = txSlice();
            if (!tx || !isCwMode(tx->mode())) {
                emitInterlockNotification(
                    tr("CW text stopped: the transmit slice is not in a CW mode."),
                    QStringLiteral("cw-text-mode:%1").arg(tx ? tx->mode() : QString()),
                    tx ? tx->panId() : QString());
                refused();
                return;
            }
            if (!m_transmitModel.admitsCwKeyEdge(true)) {
                emitInterlockNotification(tr("CW text stopped: TUNE is active."),
                                          QStringLiteral("cw-text-tuning"), tx->panId());
                refused();
                return;
            }
            // Break-in turned off, or MOX/PTT released, mid-message: the
            // backend would not raise MOX, so the element would not go out.
            if (!m_transmitModel.cwBreakIn() && !m_transmitModel.isTransmitting()) {
                emitInterlockNotification(
                    tr("CW text stopped: break-in is off and transmit is not keyed."),
                    QStringLiteral("cw-text-unkeyed"), tx->panId());
                refused();
                return;
            }
        }
        const bool wasHostEdge = std::exchange(m_hostCwTextEdge, hostTextGates);
        const bool sent = sendCwInput(down, false, false, source, traceId, sourceMs, scheduledAt, &request);
        m_hostCwTextEdge = wasHostEdge;
        if (!sent) {
            refused();
        } else if (down && (hostTextGates || !producerCwPaddleHeld(request))) {
            // Host CW text is sent text, not a held input, like CWX: its
            // keyer owns its producer, and no element holds the time-out latch.
            // Nor does an iambic element the keyer sends after its paddle let
            // go (a squeezed or memorised one): only a held paddle holds.
            noteTxInputReleased(request);
        }
    }, Qt::QueuedConnection);
}

bool RadioModel::producerCwPaddleHeld(const TxCoordinator::Request& anyRequestOfProducer) const
{
    return std::any_of(m_producerCwPaddleInputs.begin(), m_producerCwPaddleInputs.end(),
        [&anyRequestOfProducer](const TxCoordinator::Request& existing) {
            return existing.valid() && existing.producerKey() == anyRequestOfProducer.producerKey();
        });
}

void RadioModel::setProducerCwPaddleHeld(const TxCoordinator::Request& input, bool held)
{
    std::erase_if(m_producerCwPaddleInputs, [&input](const TxCoordinator::Request& existing) {
        return !existing.valid() || existing.sameRequest(input);
    });
    if (held && input.valid()) {
        m_producerCwPaddleInputs.push_back(input);
        return;
    }
    // Every paddle of this producer up releases the time-out latch; the
    // keyer's element key-ups (queueProducerCwKeyEdge) never do.
    if (!held && !producerCwPaddleHeld(input)) {
        noteTxInputReleased(input);
    }
}

} // namespace AetherSDR
