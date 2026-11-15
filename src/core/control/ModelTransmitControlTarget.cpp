#include "TransmitControlTarget.h"

#include "models/RadioModel.h"
#include "models/TxController.h"

#include <QPointer>
#include <QThread>

namespace AetherSDR::control {
namespace {

class ModelTransmitControlTarget final : public TransmitControlTarget {
public:
    explicit ModelTransmitControlTarget(RadioModel* radio)
        : m_radio(radio), m_grants(radio->independentTxGrants())
    {
        connect(radio, &RadioModel::transmitSessionInvalidated,
                this, &TransmitControlTarget::radioInvalidated);
    }
    TxGrantManager* grants() const override { return m_grants; }
    bool ready() const override { return m_radio && m_radio->independentTxReady(); }
    bool recovering() const override { return !m_radio || m_radio->transmitRecovering(); }
    bool start(const TxGrantManager::Admission& admission) override
    {
        return m_radio && TxController::fromGrantedPtt(m_radio, admission.input, admission.operation).start();
    }
    void emergencyStop() override
    {
        if (m_radio) {
            m_radio->emergencyTransmitStop();
        }
    }
private:
    QPointer<RadioModel> m_radio;
    QPointer<TxGrantManager> m_grants;
};

} // namespace

std::unique_ptr<TransmitControlTarget> makeModelTransmitControlTarget(RadioModel* radio)
{
    if (!radio || radio->thread() != QThread::currentThread()
        || radio->isConnected() || radio->isConnectAttemptInFlight()) {
        return {};
    }
    return std::make_unique<ModelTransmitControlTarget>(radio);
}

} // namespace AetherSDR::control
