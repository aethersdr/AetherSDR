#include "RadioModel.h"
#include "AetherPDMeasurementModel.h"
#include "AetherPDInstallationModel.h"
#include "SliceModel.h"

#include <QPointer>
#include <QThread>

namespace AetherSDR {

AetherPDMeasurementModel& RadioModel::aetherPDMeasurementModel()
{
    if (!m_aetherPDMeasurementModel) {
        m_aetherPDMeasurementModel = std::make_unique<AetherPDMeasurementModel>(*this);
    }
    return *m_aetherPDMeasurementModel;
}

AetherPDInstallationModel& RadioModel::aetherPDInstallationModel()
{
    if (!m_aetherPDInstallationModel) {
        m_aetherPDInstallationModel = std::make_unique<AetherPDInstallationModel>(*this);
    }
    return *m_aetherPDInstallationModel;
}

bool RadioModel::attachAetherPD(const RadioControlSession& session, const QString& adminToken)
{
    if (QThread::currentThread() != thread() || !controlSessionCurrent(session)
        || !sessionTransportCurrent() || !m_sessionCommands.current()
        || m_guiClientRegistrationState.phase() != GuiClientRegistrationState::Phase::Registered) {
        return false;
    }
    const quint32 handle = clientHandle();
    const QString clientId = m_clientInfoMap.value(handle).clientId;
    const QString serial = m_lastInfo.serial;
    const QHostAddress address = radioAddress();
    if (!handle || clientId.isEmpty() || serial.isEmpty() || address.isNull()) {
        return false;
    }
    const RadioControlFence connected = sessionControlFence();
    const QPointer<RadioModel> owner(this);
    m_aetherPDModel->setSession(address, adminToken, [owner, connected, handle, clientId, serial, address] {
        return owner && connected() && owner->clientHandle() == handle
            && owner->m_guiClientRegistrationState.phase() == GuiClientRegistrationState::Phase::Registered
            && owner->m_clientInfoMap.value(handle).clientId == clientId
            && owner->m_lastInfo.serial == serial && owner->radioAddress() == address;
    }, aetherpd::StartIdentity{clientId, handle});
    return owner && m_aetherPDModel->active();
}

QString RadioModel::aetherPDStartUnavailableReason(const RadioControlSession& session) const
{
    if (QThread::currentThread() != thread() || !controlSessionCurrent(session)
        || !sessionTransportCurrent() || !m_sessionCommands.current()) {
        return tr("A current radio session is required before starting AetherPD.");
    }
    const SliceModel* receiver = nullptr;
    for (const SliceModel* candidate : m_slices) {
        if (candidate && candidate->isActive() && sliceOwnedByUs(candidate->sliceId())) {
            if (receiver) { return tr("Select one active receiver before starting AetherPD."); }
            receiver = candidate;
        }
    }
    if (!receiver) { return tr("Select an active receiver owned by this connection before starting AetherPD."); }
    if (receiver->mode() != QStringLiteral("USB")
        || receiver->receiveObservation().mode != QStringLiteral("USB")) {
        return tr("Select USB on the active receiver and wait for radio readback before starting AetherPD.");
    }
    return {};
}

bool RadioModel::requestAetherPD(const RadioControlSession& session, aetherpd::Operation operation,
                                 std::optional<quint32> toneSpacingHz, std::optional<aetherpd::IQSelection> selection)
{
    if (QThread::currentThread() != thread() || !controlSessionCurrent(session)
        || !sessionTransportCurrent() || !m_sessionCommands.current()) {
        return false;
    }
    if (selection) {
        if (operation != aetherpd::Operation::Start || !selection->valid()) { return false; }
        bool matched = false;
        for (const PanadapterModel* pan : m_panadapters) {
            if (pan && pan->centerKnown() && pan->panStreamId() == selection->pan && pan->ownerHandle() == clientHandle()
                && pan->ownerHandle() && pan->daxiqChannel() == int(selection->channel)) { matched = true; break; }
        }
        if (!matched) { return false; }
    }
    if (operation == aetherpd::Operation::Start && !aetherPDStartUnavailableReason(session).isEmpty()) {
        return false;
    }
    return m_aetherPDModel->request(operation, toneSpacingHz, selection);
}

std::function<bool()> RadioModel::bindAetherPDCapture(const RadioControlSession& session,
                                                   txlin::AetherPDCaptureSource& source)
{
    if (QThread::currentThread() != thread() || !controlSessionCurrent(session)
        || !sessionTransportCurrent() || !m_sessionCommands.current()) {
        return {};
    }
    return m_aetherPDModel->bindCaptureSource(source);
}

std::function<bool()> RadioModel::bindAetherPDTest(const RadioControlSession& session,
                                                txlin::AetherPDTestClient& client)
{
    if (QThread::currentThread() != thread() || !controlSessionCurrent(session)
        || !sessionTransportCurrent() || !m_sessionCommands.current()) {
        return {};
    }
    return m_aetherPDModel->bindTestClient(client);
}

bool RadioModel::detachAetherPD(const RadioControlSession& session)
{
    if (QThread::currentThread() != thread() || session != controlSession()) {
        return false;
    }
    m_aetherPDModel->clearSession();
    return true;
}

} // namespace AetherSDR
