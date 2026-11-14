#pragma once

#include "core/backends/flex/FlexSetupStatus.h"
#include "models/UsbCableModel.h"

namespace AetherSDR {
class FlexSetupStatusTestPeer {
public:
    // Through the backend's status ingress when the model has one, as a radio
    // line arrives; otherwise decoded here and stamped with the model's session.
    template<typename Model>
    static void deliver(Model& model, const QString& object, const QMap<QString, QString>& fields)
    {
        if (model.m_statusIngress) {
            model.handleStatusForTest(object, fields);
            return;
        }
        auto observation = decodeFlexSetupStatus(object, fields);
        if (!observation) { return; }
        // Without an ingress the session opens at connect, which a fixture skips.
        if (!model.m_observationSessionOpen) { model.beginStatusSession(); }
        observation->statusEpoch = model.m_observationEpoch;
        model.applySetupObservation(*observation);
    }
    template<typename Model>
    static bool send(Model& model, const std::shared_ptr<SetupStatusLifetime>& identity)
    { return model.sendSetupEntityCommand(QStringLiteral("xvtr set 2 rx_only=1"), {}, identity); }
    template<typename Model> static void reset(Model& model) { model.resetSetupObservations(); }
    template<typename Model> static void refreshLicense(Model& model, const QDateTime& now)
    { model.refreshLicenseSubscription(now); }
    template<typename Model> static void connectionError(Model& model)
    { model.onConnectionError(QStringLiteral("timeout")); }
    // A SmartLink session over the model's ingress, bound as the WAN connect binds it.
    template<typename Model, typename Wan> static void connectWan(Model& model, Wan* wan)
    {
        model.m_wanConn = wan;
        model.openObservationSession(model.m_statusEpoch = model.m_statusIngress->bindLive(wan));
        model.m_connectedStatusEpoch = model.m_statusEpoch;
    }
    template<typename Model> static void releaseWan(Model& model) { model.releaseWanStatus(); model.m_wanConn = nullptr; }
};

inline void applyCableStatus(UsbCableModel& model, const QString& serial, const QMap<QString, QString>& fields)
{
    const auto observation = decodeFlexSetupStatus(QStringLiteral("usb_cable ") + serial, fields);
    if (observation) { model.applyObservation(observation->usbCable); }
}
} // namespace AetherSDR
