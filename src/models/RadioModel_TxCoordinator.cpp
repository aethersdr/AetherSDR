#include "RadioModel.h"
#include "TxController.h"
#include "core/LogManager.h"

#include <QPointer>
#include <QScopeGuard>

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
    const TxCoordinator::Producer producer = m_txCoordinator.registerProducer(continuousMicrophone);
    connect(lifetime, &QObject::destroyed, this, [producer] {
        producer.invalidate();
    }, Qt::DirectConnection); // atomic invalidation; never touches a model
    return producer;
}

TxCoordinator::Producer RadioModel::registerTxProducer()
{
    return QThread::currentThread() == thread() ? m_txCoordinator.registerProducer() : TxCoordinator::Producer{};
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

void RadioModel::abortProducerPtt(const TxCoordinator::Request& request,
                                 TransmitModel::PttSource source)
{
    if (QThread::currentThread() != thread()) {
        return;
    }
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

bool RadioModel::beginTxActivity(TxActivity activity, const TxCoordinator::Request* request)
{
    if (m_txInputsStopping) {
        return false;
    }
    if (request && !m_txCoordinator.acceptsRequest(*request)) {
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
        emitInterlockNotification(tr("Transmit is unavailable while the radio disconnects."),
                                  QStringLiteral("tx-session-closing"));
        return false;
    }
    if (!m_backend || !refuseKeyOnTransmitIncapableBackend()
        || !refuseKeyInReceiveOnlyMode()) {
        return false;
    }
    const RadioCapabilities caps = backendCapabilities();
    if ((activity == TxActivity::Cwx && !caps.hasRadioSideCwKeyer)
        || (activity == TxActivity::Atu && !caps.hasTuner)) {
        emitInterlockNotification(tr("This radio does not support the requested transmit operation."),
                                  QStringLiteral("tx-operation-unsupported"));
        return false;
    }
    const QString gate = activity == TxActivity::Tune || activity == TxActivity::Atu
        ? QStringLiteral("tune-start")
        : activity == TxActivity::Mox ? QStringLiteral("xmit") : QStringLiteral("cw-key");
    if (transmitStartBlockedByInhibit(gate)) {
        return false;
    }
    if (!m_backendTxProducer.valid()) {
        m_backendTxProducer = registerTxProducer(m_backend.get());
    }
    if (!m_backendTxProducer.valid()) {
        emitInterlockNotification(tr("Transmit producer capacity is exhausted."),
                                  QStringLiteral("tx-producer-capacity"));
        return false;
    }
    const TxCoordinator::Admission admission = m_txCoordinator.acquire(m_desktopTxActor, txMonotonicMs());
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
    m_backend->setTransmitContext(m_txCoordinator.mediaContext(m_backendTxProducer,
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
    return true;
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
    bool dispatched = false;
    const TransmitModel::KeyingRoute route = producerKeyingRoute(request, TxActivity::Atu, dispatched);
    m_transmitModel.requestAtu(start, route);
    const bool accepted = dispatched;
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
    if (!on && !request) {
        (void)m_txCoordinator.requestIntentEnd(intent);
    }
    if (on) {
        armInterlockNotification(m_transmitModel.activePttSource());
        applyTuneInhibit();
    }
    const TxCoordinator::Operation cleanup = request ? operation : m_txCoordinator.cleanupFence();
    bool releaseQueued = false;
    bool dispatched = false;
    if (m_backend && commandEpoch == m_tuneCommandEpoch
        && (on ? operation.permitsDispatch(txMonotonicMs()) : cleanup.permitsCleanup())) {
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
        applyTuneInhibit();
    }
    const TxCoordinator::Operation cleanup = request ? operation : m_txCoordinator.cleanupFence();
    bool releaseQueued = false;
    bool dispatched = false;
    if (m_backend && commandEpoch == m_atuCommandEpoch
        && (start ? operation.permitsDispatch(txMonotonicMs()) : cleanup.permitsCleanup())) {
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
    }
}

unsigned RadioModel::activeTxActivities() const
{
    // Include older draining contributions, not just the current compatibility
    // slot. A later completed edge cannot hide an earlier pending CW tail.
    return m_txCoordinator.activeActivities(m_txOperation);
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
        ++m_pendingTxDeliveries;
    }
    return [this, operation, tracked] {
        if (!tracked || !m_txOperation.sameOperation(operation)
            || m_pendingTxDeliveries == 0) {
            return;
        }
        --m_pendingTxDeliveries;
        completeLocalTxIfDrained();
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

void RadioModel::sendTxKeyingCommand(const QString& command, const TxCoordinator::Command& fence)
{
    const TxCoordinator::Operation operation = fence.operation;
    const auto completed = [completion = fence.completion] { completion.finish(); };
    if (fence.keying) {
        if (!sendTxTcpCommand(command, operation, true, completed)) {
            completed();
        }
        return;
    }
    // Retain a short, normally released operation until its queued key-up is
    // consumed, just as NetCW does. Otherwise completion cancels an earlier
    // key-on before the transport has had a chance to consume either edge.
    // This is local queue completion, not qualified radio-idle evidence.
    if (!sendTxTcpCommand(command, operation, false, completed)) {
        completed();
    }
}

bool RadioModel::requestProducerCwx(const TxCoordinator::Request& request, const QString& text,
                                   std::function<void()> onAdmitted, int macroIndex, bool live)
{
    const QPointer<RadioModel> self(this);
    if (QThread::currentThread() != thread()) {
        return false;
    }
    if (macroIndex < 0 || macroIndex > 12 || (macroIndex == 0 && !cwTextValidationError(text).isEmpty())) {
        abortProducerCwx(request);
        return false;
    }
    bool admitted = false;
    const CwxModel::TransmissionRoute route{
        [self, request, &admitted, &onAdmitted]() -> CwxModel::TransmissionPermit {
            if (!self || !self->beginTxActivity(TxActivity::Cwx, &request) || !self) {
                return {};
            }
            admitted = true;
            const TxCoordinator::Operation operation = self->m_txCoordinator.requestOperation(request);
            if (onAdmitted) {
                onAdmitted();
            }
            return [operation] { return operation.permitsDispatch(txMonotonicMs()); };
        },
        [self, request](const QString& value, int) {
            return self && self->dispatchCwxText(value, self->m_txCoordinator.requestOperation(request));
        },
        [self, request](const QString& command, int epoch, int nChars) {
            if (self) { self->dispatchCwxCommand(command, self->m_txCoordinator.requestOperation(request), epoch, nChars); }
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
    if (self && !admitted) {
        abortProducerCwx(request);
    }
    return admitted;
}

void RadioModel::abortProducerCwx(const TxCoordinator::Request& request)
{
    if (QThread::currentThread() != thread()) {
        return;
    }
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
        // This includes the unacknowledged radio-side text tail after local
        // handoff, but never a replacement producer's queue, even in one over.
        m_cwxModel.clearBuffer();
    }
}

void RadioModel::dispatchCwxCommand(const QString& command, TxCoordinator::Operation operation,
                                    int epoch, int nChars)
{
    if (!usesFlexCommandPlane()) {
        return;
    }
    if (command.startsWith("cwx send") || command.startsWith("cwx macro send")) {
        if (!operation.permitsDispatch(txMonotonicMs())) {
            return;
        }
        m_cwxActive = true;
        if (nChars >= 0) {
            // The epoch and original producer survive QSK gaps and nested
            // notifications; an earlier reply cannot adopt a new text batch.
            m_cwxDrainArmed = true;
            sendCwxCommand(command, true, operation,
                [this, operation, epoch, nChars](int result, const QString& body) {
                    if (operation.permitsDispatch(txMonotonicMs())) {
                        m_cwxModel.handleSendReply(result, body, epoch, nChars);
                    }
                });
        } else {
            sendCwxCommand(command, true, operation);
        }
    } else if (command.startsWith("cwx clear")) {
        m_cwxActive = false;
        m_cwxDrainArmed = false;
        sendCwxCommand(command, false, operation);
    } else {
        sendCmd(command);
    }
}

bool RadioModel::dispatchCwxText(const QString& text, TxCoordinator::Operation original)
{
    if (usesFlexCommandPlane()) {
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
    if (!usesFlexCommandPlane() || untrackedMacro) {
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

void RadioModel::sendCwxCommand(const QString& command, bool keying,
                                const TxCoordinator::Operation& operation, ResponseCallback reply)
{
    const TxCoordinator::Operation fence = keying || operation.permitsCleanup()
        ? operation : m_txCoordinator.cleanupFence();
    const TxCoordinator::Completion completion = trackCwxQueue(operation);
    const auto consumed = [completion] { completion.finish(); };
    // An ESC must cancel queued text even while a separate MOX intent keeps
    // the shared desktop operation alive. Never read CwxModel on the worker.
    const auto batch = m_cwxModel.queuedTransmissionPermit();
    if (!sendTxTcpCommand(command, fence, keying, consumed, std::move(reply), batch)) {
        consumed();
    }
}

void RadioModel::stopTxOperation(const TxCoordinator::Operation& operation,
                               TxCoordinator::StopReason reason)
{
    Q_UNUSED(reason);
    // TxCoordinator has already invalidated the keying fence and entered
    // recovery. All cleanup below is key-up/bypass/abort; never re-admit it.
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
}

void RadioModel::cancelLocalTransmit()
{
    if (QThread::currentThread() != thread()) {
        return;
    }
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

void RadioModel::resetTxOperations()
{
    // Even an idle coordinator must refuse new intent while the session dies.
    // Close admission BEFORE cancellation/reply/model notifications can reenter
    // us; operation recovery alone only covers a previously active operation.
    m_txSessionClosing = true;
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
                                       std::chrono::steady_clock::time_point scheduledAt)
{
    QMetaObject::invokeMethod(this, [this, request, down, source, traceId, sourceMs, scheduledAt] {
        if (m_txSessionClosing || scheduledAt < m_cwInputNotBefore) {
            return;
        }
        (void)sendCwInput(down, false, false, source, traceId, sourceMs, scheduledAt, &request);
    }, Qt::QueuedConnection);
}

void RadioModel::setProducerCwPaddleHeld(const TxCoordinator::Request& input, bool held)
{
    std::erase_if(m_producerCwPaddleInputs, [&input](const TxCoordinator::Request& existing) {
        return !existing.valid() || existing.sameRequest(input);
    });
    if (held && input.valid()) {
        m_producerCwPaddleInputs.push_back(input);
    }
}

} // namespace AetherSDR
