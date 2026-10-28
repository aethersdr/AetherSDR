#include "RadioModel.h"

#include <QThread>
#include <QTimer>

namespace AetherSDR {

FileTransferHandle RadioModel::beginFileTransfer(FileTransferPurpose purpose, QObject* owner,
    const RadioControlSession& expectedSession, int timeoutMs)
{
    if (QThread::currentThread() != thread()) { return {}; }
    const auto control = backendCapabilities().fileTransferControl;
    if (!owner || owner->thread() != thread() || !control
        || !control->supports(purpose) || !controlSessionCurrent(expectedSession)
        || radioAddress().isNull() || timeoutMs <= 0 || timeoutMs > 2 * 60 * 60 * 1000) { return {}; }
    if (const auto previous = m_fileTransferOperation; previous
        && controlSessionCurrent(previous->m_session)
        && (previous->current() || !previous->m_settleDeadline.hasExpired())) { return {}; }
    if (firmwareRetryBlocked()) { return {}; }
    if (!fileTransferEligible(purpose, false)) { return {}; }
    auto operation = std::make_shared<FileTransferOperation>();
    operation->m_guard = FileTransferGuard(timeoutMs);
    operation->m_session = expectedSession;
    operation->m_peer = radioAddress();
    operation->m_owner = owner;
    operation->m_purpose = purpose;
    m_fileTransferOperation = operation;
    const std::weak_ptr<FileTransferOperation> weak = operation;
    connect(owner, &QObject::destroyed, this, [weak] {
        if (const auto active = weak.lock()) { active->cancel(); }
    });
    const auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(this, &RadioModel::connectionStateChanged, owner, [this, weak, connection] {
        const auto active = weak.lock();
        if (!active || !fileTransferCurrent(active)) {
            if (active) { active->cancel(); }
            QObject::disconnect(*connection);
        }
    });
    QTimer::singleShot(timeoutMs, owner, [weak, connection] {
        if (const auto active = weak.lock()) { active->cancel(); }
        QObject::disconnect(*connection);
    });
    return operation;
}

bool RadioModel::fileTransferCurrent(const FileTransferHandle& operation) const
{
    return operation && operation->current() && operation->m_owner
        && controlSessionCurrent(operation->m_session)
        && transferPeerMatches(radioAddress(), operation->peer())
        && m_fileTransferOperation == operation;
}

bool RadioModel::fileTransferEligible(FileTransferPurpose purpose, bool holdingReservation) const
{
    using Purpose = FileTransferPurpose;
    if ((purpose == Purpose::ProfileMetadata || purpose == Purpose::ProfileExport
         || purpose == Purpose::ProfileImport) && (isWan() || isProfileTransferBlocked())) { return false; }
    if (purpose == Purpose::DvkUpload || purpose == Purpose::DvkDownload) {
        if (!hasVoiceKeyer() || dvkLicenseRefused()) { return false; }
        if (!holdingReservation) { return m_dvkModel.canStartOperation(); }
        return (m_dvkModel.status() == VoiceKeyer::Idle || m_dvkModel.status() == VoiceKeyer::Unknown)
            && m_dvkModel.pendingOperation() == VoiceKeyer::Unknown;
    }
    return purpose != Purpose::DockerWaveform
        || (m_flexWaveformModel.wfpStatusSeen() && m_flexWaveformModel.wfpPowered()
            && m_flexWaveformModel.wfpReady());
}

void RadioModel::finishFileTransfer(const FileTransferHandle& operation, int settleMs)
{
    if (!operation) { return; }
    operation->cancel();
    if (m_fileTransferOperation == operation && controlSessionCurrent(operation->m_session)) {
        operation->m_settleDeadline = QDeadlineTimer(qBound(0, settleMs, 5000));
    }
}

bool RadioModel::firmwareRetryBlocked() const
{
    return m_firmwareRetryBlocked || (m_fileTransferOperation
        && controlSessionCurrent(m_fileTransferOperation->m_session)
        && m_fileTransferOperation->guard().firmwarePending());
}

void RadioModel::clearFirmwareRetryBlock()
{
    m_firmwareRetryBlocked = false;
    if (m_fileTransferOperation) { m_fileTransferOperation->guard().setFirmwarePending(false); }
}

ReceiveDispatch RadioModel::requestFileTransfer(const FileTransferHandle& operation,
    const FileTransferRequest& request, const FileTransferCompletion& completion)
{
    const auto refuse = [&] {
        if (completion) { completion({{RadioControlReply::Kind::Refused, -1,
            QStringLiteral("Transfer unavailable, busy, or original session ended.")}, 0}); }
        return ReceiveDispatch::Unsupported;
    };
    if (QThread::currentThread() != thread() || !fileTransferCurrent(operation)
        || !m_backend || !request.valid()) { return refuse(); }
    const auto control = backendCapabilities().fileTransferControl;
    if (!control || !control->supports(request.purpose)
        || !fileTransferEligible(request.purpose, true)) { return refuse(); }
    const bool followup = operation->m_purpose == FileTransferPurpose::ProfileMetadata
        && request.purpose == FileTransferPurpose::ProfileExport && operation->m_metadataAccepted;
    if (!followup && (operation->m_requested || operation->m_purpose != request.purpose)) { return refuse(); }
    if (followup) { operation->m_purpose = request.purpose; }
    operation->m_requested = true;
    const QPointer<RadioModel> model(this);
    const QPointer<QObject> owner(operation->m_owner);
    const auto delivered = std::make_shared<bool>(false);
    const FileTransferCompletion reply = [model, owner, operation, delivered, completion, purpose = request.purpose]
        (const FileTransferReply& result) {
        if (*delivered) { return; }
        *delivered = true;
        if (!model || !owner) { return; }
        if (!model->fileTransferCurrent(operation)) {
            if (completion) { completion({{RadioControlReply::Kind::Cancelled, -1,
                QStringLiteral("Original transfer ended.")}, 0}); }
            return;
        }
        operation->m_metadataAccepted = purpose == FileTransferPurpose::ProfileMetadata
            && result.result.kind == RadioControlReply::Kind::Accepted;
        if (completion) { completion(result); }
    };
    const RadioControlFence current = [model, operation, purpose = request.purpose] {
        return model && model->fileTransferCurrent(operation)
            && model->fileTransferEligible(purpose, true);
    };
    QTimer::singleShot(30000, owner, [reply] {
        reply({{RadioControlReply::Kind::Failed, -1, QStringLiteral("Transfer setup timed out.")}, 0});
    });
    const ReceiveDispatch result = m_backend->requestFileTransfer(request, reply, operation->guard(), current);
    if (result == ReceiveDispatch::Unsupported) {
        reply({{RadioControlReply::Kind::Refused, -1, QStringLiteral("Transfer is unsupported.")}, 0});
    }
    return result;
}

ReceiveDispatch RadioModel::requestWaveform(const WaveformRequest& request, QObject* owner,
    const RadioControlCompletion& completion)
{
    if (QThread::currentThread() != thread()) { return ReceiveDispatch::Unsupported; }
    const auto control = backendCapabilities().waveformControl;
    if (!request.valid() || !owner || owner->thread() != thread() || !m_backend
        || !control || !control->supports(request.action) || !controlSessionCurrent(controlSession())) {
        if (completion) { completion({RadioControlReply::Kind::Refused, -1,
            QStringLiteral("Waveform control is unavailable.")}); }
        return ReceiveDispatch::Unsupported;
    }
    const FileTransferGuard guard(30000);
    connect(owner, &QObject::destroyed, this, [guard] { guard.cancel(); });
    connect(m_backend.get(), &QObject::destroyed, this, [guard] { guard.cancel(); });
    const RadioControlSession session = controlSession();
    const QPointer<RadioModel> model(this);
    const QPointer<QObject> caller(owner);
    const RadioControlFence current = [model, caller, session, guard] {
        return model && caller && guard.current() && model->controlSessionCurrent(session);
    };
    const RadioControlCompletion reply = fencedSetupCompletion(owner,
        [guard, completion](const RadioControlReply& result) {
            guard.cancel();
            if (completion) { completion(result); }
        });
    const ReceiveDispatch result = m_backend->requestWaveform(request, reply, guard, current);
    if (result == ReceiveDispatch::Unsupported) {
        reply({RadioControlReply::Kind::Refused, -1, QStringLiteral("Waveform control is unsupported.")});
    }
    return result;
}

} // namespace AetherSDR
