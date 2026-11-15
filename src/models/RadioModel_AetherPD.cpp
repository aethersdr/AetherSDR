#include "RadioModel.h"
#include "AetherPDMeasurementModel.h"

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

bool RadioModel::requestAetherPD(const RadioControlSession& session, aetherpd::Operation operation)
{
    if (QThread::currentThread() != thread() || !controlSessionCurrent(session)
        || !sessionTransportCurrent() || !m_sessionCommands.current()) {
        return false;
    }
    return m_aetherPDModel->request(operation);
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
