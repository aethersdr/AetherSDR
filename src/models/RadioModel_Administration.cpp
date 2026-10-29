#include "RadioModel.h"

#include <QPointer>
#include <QThread>
#include <QTimer>

namespace AetherSDR {
namespace {
int administrationIndex(RadioSetupField field)
{
    if (field == RadioSetupField::RemoteOnEnabled) { return 0; }
    if (field == RadioSetupField::MultiFlexEnabled) { return 1; }
    return -1;
}
}

struct RadioModel::CwAutoTuneOperation {
    QPointer<SliceModel> slice;
    RadioControlSession session{};
    quint64 revision{0};
    bool loopRequested{false};
    bool cleanupEligible{false};
    bool cleanupIssued{false};
};

bool RadioModel::remoteOnRequested() const
{
    return m_administrationPending[0].value_or(m_remoteOnEnabled);
}

bool RadioModel::multiFlexRequested() const
{
    return m_administrationPending[1].value_or(m_multiFlexEnabled);
}

bool RadioModel::administrationSettingAvailable(RadioSetupField field) const
{
    const int index = administrationIndex(field);
    const auto control = backendCapabilities().radioSetupControl;
    return index >= 0 && !m_resettingAdministration && controlSessionCurrent(controlSession())
        && control && control->supports(field) && m_administrationObserved[index];
}

ReceiveDispatch RadioModel::requestAdministrationSetting(const RadioSetupRequest& request,
    QObject* context, const RadioControlCompletion& completion)
{
    const int index = administrationIndex(request.field);
    if (QThread::currentThread() != thread() || index < 0 || !request.valid()
        || !administrationSettingAvailable(request.field)) {
        if (completion) {
            completion({RadioControlReply::Kind::Refused, -1, tr("Radio setting is not available.")});
        }
        return ReceiveDispatch::Unsupported;
    }
    const QPointer<RadioModel> alive(this);
    const QPointer<QObject> owner(context ? context : this);
    if (context && context != this) {
        connect(context, &QObject::destroyed, this, &RadioModel::reconcileAdministration, Qt::UniqueConnection);
    }
    const RadioControlSession session = controlSession();
    const quint64 revision = ++m_administrationRevisions[index];
    m_administrationPending[index] = std::get<bool>(request.value);
    reconcileAdministration();
    if (!alive) { return ReceiveDispatch::Unsupported; }
    const RadioControlFence current = [alive, owner, session, index, revision] {
        return alive && owner && !alive->m_resettingAdministration
            && alive->controlSessionCurrent(session)
            && alive->m_administrationRevisions[index] == revision;
    };
    const auto finished = std::make_shared<bool>(false);
    const RadioControlCompletion reply = [alive, owner, session, index, revision, finished, completion]
        (const RadioControlReply& result) {
        if (*finished) { return; }
        *finished = true;
        if (!alive) { return; }
        if (alive->controlSession() == session && alive->m_administrationRevisions[index] == revision) {
            alive->m_administrationPending[index].reset();
            emit alive->infoChanged();
        }
        if (alive && owner && completion) { completion(result); }
    };
    const ReceiveDispatch result = current()
        ? m_backend->requestRadioSetup(request, reply, current) : ReceiveDispatch::Unsupported;
    if (!alive) { return result; }
    if (result != ReceiveDispatch::Dispatched) {
        reply({RadioControlReply::Kind::Refused, -1, tr("Radio setting was not dispatched.")});
    }
    if (alive) { emit infoChanged(); }
    return result;
}

void RadioModel::setRemoteOnEnabled(bool enabled)
{
    requestRadioSetup({RadioSetupField::RemoteOnEnabled, enabled}, this,
        [this](const RadioControlReply& reply) {
            if (reply.kind == RadioControlReply::Kind::Failed || reply.kind == RadioControlReply::Kind::Refused) {
                emit commandDropped(tr("Remote On: %1").arg(reply.message));
            }
        });
}

void RadioModel::setMultiFlexEnabled(bool enabled)
{
    requestRadioSetup({RadioSetupField::MultiFlexEnabled, enabled}, this,
        [this](const RadioControlReply& reply) {
            if (reply.kind == RadioControlReply::Kind::Failed || reply.kind == RadioControlReply::Kind::Refused) {
                emit commandDropped(tr("multiFLEX: %1").arg(reply.message));
            }
        });
}

void RadioModel::administrationWriteResult(quint64 ticket, bool written)
{
    const auto operation = m_administrationWrites.value(ticket);
    if (!operation) { return; }
    if (!written) {
        finishAdministrationWrite(ticket, {RadioControlReply::Kind::Refused, -1,
            tr("Command was cancelled or the transport could not write it.")});
        return;
    }
    if (operation->delivered) { return; }
    operation->delivered = true;
    if (operation->current() && controlSessionCurrent(operation->session) && operation->written) {
        operation->written();
    }
}

void RadioModel::finishAdministrationWrite(quint64 ticket, const RadioControlReply& reply)
{
    const auto operation = m_administrationWrites.value(ticket);
    if (!operation) { return; }
    const QPointer<RadioModel> alive(this);
    // A reply can arrive before a queued write receipt; it proves dispatch.
    if (reply.code >= 0 && (reply.kind == RadioControlReply::Kind::Accepted
                          || reply.kind == RadioControlReply::Kind::Failed)) {
        administrationWriteResult(ticket, true);
    }
    if (!alive || m_administrationWrites.value(ticket) != operation) { return; }
    m_administrationWrites.remove(ticket);
    operation->guard.cancel();
    if (operation->deadline) {
        operation->deadline->stop();
        operation->deadline->deleteLater();
    }
    forgetAdministrationReply(*operation);
    if (operation->completion) { operation->completion(reply); }
}

bool RadioModel::rebootRadio(std::optional<RadioControlSession> expectedSession)
{
    const RadioControlSession session = controlSession();
    if (QThread::currentThread() != thread() || m_resettingAdministration || !m_backend
        || !controlSessionCurrent(session) || (expectedSession && *expectedSession != session)
        || !backendCapabilities().canReboot || m_rebootQueued || m_rebootInProgress) { return false; }
    const QPointer<RadioModel> alive(this);
    const quint64 revision = ++m_rebootRevision;
    m_rebootQueued = true;
    m_rebootRadioSerial = serial();
    const RadioControlFence current = [alive, session, revision] {
        return alive && !alive->m_resettingAdministration
            && alive->controlSessionCurrent(session) && alive->m_rebootRevision == revision;
    };
    const std::function<void()> written = [alive, session, revision] {
        if (!alive || !alive->controlSessionCurrent(session) || alive->m_rebootRevision != revision) { return; }
        alive->m_rebootQueued = false;
        alive->m_rebootInProgress = true;
        QTimer::singleShot(250, alive, [alive, session, revision] {
            if (alive && alive->controlSessionCurrent(session) && alive->m_rebootRevision == revision
                && alive->m_rebootInProgress) { alive->forceDisconnect(); }
        });
        QTimer::singleShot(60000, alive, [alive, revision] {
            if (alive && alive->m_rebootRevision == revision) {
                alive->m_rebootInProgress = false;
                alive->m_rebootRadioSerial.clear();
            }
        });
    };
    const RadioControlCompletion reply = [alive, revision](const RadioControlReply& result) {
        if (!alive || alive->m_rebootRevision != revision) { return; }
        alive->m_rebootQueued = false;
        if (result.kind == RadioControlReply::Kind::Refused
            || (result.kind == RadioControlReply::Kind::Failed && result.code >= 0)) {
            ++alive->m_rebootRevision;
            alive->m_rebootInProgress = false;
            alive->m_rebootRadioSerial.clear();
            emit alive->commandDropped(tr("Radio reboot: %1").arg(result.message));
        }
    };
    const ReceiveDispatch result = m_backend->requestRadioReboot(reply, current, written);
    if (alive && result != ReceiveDispatch::Dispatched) {
        reply({RadioControlReply::Kind::Refused, -1, tr("Reboot was not dispatched.")});
    }
    return result == ReceiveDispatch::Dispatched;
}

bool RadioModel::cwAutoTuneLoopRequested(int sliceId) const
{
    const auto operation = m_cwAutoTuneOperations.value(sliceId);
    return operation && operation->slice && operation->slice == slice(sliceId)
        && controlSessionCurrent(operation->session) && operation->loopRequested;
}

QString RadioModel::cwAutoTuneUnavailableReason(SliceModel* source, CwAutoTuneAction action) const
{
    if (!validCwAutoTuneAction(action)) { return tr("Invalid CW receive auto-tune action."); }
    if (m_resettingAdministration || !controlSessionCurrent(controlSession())) { return tr("Connect to a radio."); }
    if (!source || slice(source->sliceId()) != source || !sliceOwnedByUs(source->sliceId())) {
        return tr("CW receive auto-tune requires a current, owned slice.");
    }
    const auto capability = backendCapabilities().cwReceiveControl;
    if (!capability || !capability->supports(action)) { return tr("Not supported by this radio."); }
    const auto operation = m_cwAutoTuneOperations.value(source->sliceId());
    if (action == CwAutoTuneAction::Stop) {
        return operation && operation->slice == source && operation->session == controlSession()
                && operation->cleanupEligible
            ? QString() : tr("No CW auto-tune loop was requested by this client.");
    }
    if (!m_licenseSubscription.contains(QStringLiteral("SmartSDR+"))) { return tr("Requires SmartSDR+."); }
    const QString mode = source->mode();
    if (mode != QLatin1String("CW") && mode != QLatin1String("CWL") && mode != QLatin1String("CWU")) {
        return tr("Select a CW receive mode.");
    }
    if (source->isLocked()) { return tr("Unlock the slice to start CW receive auto-tune."); }
    return {};
}

void RadioModel::cwAutoTune(int sliceId, bool intermittent)
{
    requestCwAutoTune(slice(sliceId), intermittent ? CwAutoTuneAction::Start : CwAutoTuneAction::Stop);
}

void RadioModel::cwAutoTuneOnce(int sliceId)
{
    requestCwAutoTune(slice(sliceId), CwAutoTuneAction::Once);
}

void RadioModel::requestCwAutoTune(SliceModel* source, CwAutoTuneAction action)
{
    if (QThread::currentThread() != thread()) { return; }
    const QString reason = cwAutoTuneUnavailableReason(source, action);
    if (!reason.isEmpty()) { emit commandDropped(reason); return; }
    const int id = source->sliceId();
    const QPointer<RadioModel> alive(this);
    auto operation = m_cwAutoTuneOperations.value(id);
    if (!operation || operation->slice != source || operation->session != controlSession()) {
        operation = std::make_shared<CwAutoTuneOperation>();
        operation->slice = source;
        operation->session = controlSession();
        m_cwAutoTuneOperations.insert(id, operation);
        connect(source, &SliceModel::modeChanged, this, &RadioModel::reconcileAdministration, Qt::UniqueConnection);
        connect(source, &SliceModel::lockedChanged, this, &RadioModel::reconcileAdministration, Qt::UniqueConnection);
        connect(source, &QObject::destroyed, this, &RadioModel::reconcileAdministration, Qt::UniqueConnection);
    }
    const bool previousLoop = operation->loopRequested;
    const bool previousCleanup = operation->cleanupEligible;
    if (action != CwAutoTuneAction::Once) {
        ++operation->revision;
        operation->loopRequested = action == CwAutoTuneAction::Start;
        operation->cleanupIssued = action == CwAutoTuneAction::Stop;
        operation->cleanupEligible = true;
    }
    const quint64 revision = operation->revision;
    const RadioControlFence current = [alive, operation, revision, action] {
        return alive && operation->slice && operation->revision == revision
            && alive->controlSessionCurrent(operation->session)
            && alive->cwAutoTuneUnavailableReason(operation->slice, action).isEmpty();
    };
    reconcileAdministration();
    if (!alive) { return; }
    const RadioControlCompletion reply = [alive, operation, id, revision, action, previousLoop, previousCleanup](const RadioControlReply& result) {
        if (!alive || !operation->slice || alive->m_cwAutoTuneOperations.value(id) != operation
            || operation->revision != revision || !alive->controlSessionCurrent(operation->session)) { return; }
        if (action == CwAutoTuneAction::Stop && result.kind == RadioControlReply::Kind::Accepted) {
            operation->cleanupEligible = false;
        } else if (result.kind == RadioControlReply::Kind::Failed
                   || result.kind == RadioControlReply::Kind::Refused) {
            if (action == CwAutoTuneAction::Start
                && (result.kind == RadioControlReply::Kind::Refused || result.code >= 0)) {
                operation->loopRequested = previousLoop;
                operation->cleanupEligible = previousCleanup;
            }
            if (action == CwAutoTuneAction::Stop) { operation->loopRequested = true; }
            emit alive->commandDropped(tr("CW receive auto-tune: %1").arg(result.message));
            if (!alive) { return; }
        }
        emit alive->cwAutoTuneIntentChanged(id);
    };
    const ReceiveDispatch result = current()
        ? m_backend->requestCwAutoTune(id, action, reply, current) : ReceiveDispatch::Unsupported;
    if (!alive) { return; }
    if (result != ReceiveDispatch::Dispatched) {
        reply({RadioControlReply::Kind::Refused, -1, tr("Auto-tune was not dispatched.")});
    }
    if (alive) { emit cwAutoTuneIntentChanged(id); }
}

void RadioModel::reconcileAdministration()
{
    if (m_reconcilingAdministration || m_resettingAdministration) { return; }
    m_reconcilingAdministration = true;
    const QPointer<RadioModel> alive(this);
    const auto writes = m_administrationWrites;
    for (auto it = writes.cbegin(); it != writes.cend(); ++it) {
        if (!controlSessionCurrent(it.value()->session) || !it.value()->current()) {
            finishAdministrationWrite(it.key(), {RadioControlReply::Kind::Cancelled, -1,
                tr("Original control operation ended.")});
            if (!alive) { return; }
        }
    }
    const auto loops = m_cwAutoTuneOperations;
    for (auto it = loops.cbegin(); it != loops.cend(); ++it) {
        const auto operation = it.value();
        if (!operation->slice || !controlSessionCurrent(operation->session)
            || slice(it.key()) != operation->slice || !sliceOwnedByUs(it.key())) {
            ++operation->revision;
            m_cwAutoTuneOperations.remove(it.key());
            emit cwAutoTuneIntentChanged(it.key());
        } else if (operation->loopRequested && !operation->cleanupIssued
                   && !cwAutoTuneUnavailableReason(operation->slice, CwAutoTuneAction::Start).isEmpty()) {
            requestCwAutoTune(operation->slice, CwAutoTuneAction::Stop);
        }
        if (!alive) { return; }
    }
    m_reconcilingAdministration = false;
}

void RadioModel::resetAdministration(bool notify)
{
    if (m_resettingAdministration) { return; }
    m_resettingAdministration = true;
    const QPointer<RadioModel> alive(this);
    const auto writes = m_administrationWrites;
    for (auto it = writes.cbegin(); it != writes.cend(); ++it) {
        if (!notify) {
            it.value()->completion = {};
            it.value()->written = {};
        }
        finishAdministrationWrite(it.key(), {RadioControlReply::Kind::Cancelled, -1,
            tr("Original radio session ended.")});
        if (!alive) { return; }
    }
    const auto ids = m_cwAutoTuneOperations.keys();
    m_cwAutoTuneOperations.clear();
    m_administrationPending = {};
    m_administrationObserved = {};
    m_rebootQueued = false;
    for (int id : ids) {
        emit cwAutoTuneIntentChanged(id);
        if (!alive) { return; }
    }
    m_resettingAdministration = false;
}

} // namespace AetherSDR
