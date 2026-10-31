#pragma once

#include "core/backends/flex/FlexSetupStatus.h"
#include "models/UsbCableModel.h"

namespace AetherSDR {
class FlexSetupStatusTestPeer {
public:
    template<typename Model>
    static void deliver(Model& model, const QString& object, const QMap<QString, QString>& fields)
    {
        const auto observation = decodeFlexSetupStatus(object, fields, std::make_shared<SetupStatusLifetime>());
        if (observation) { model.applySetupObservation(*observation); }
    }
    template<typename Model> static void apply(Model& model, const SetupStatusObservation& observation)
    { model.applySetupObservation(observation); }
    template<typename Model, typename Transport>
    static void connection(Model& model, Transport* connection) { model.m_connection = connection; }
    template<typename Transport, typename Writer>
    static void writer(Transport& transport, Writer writer) { transport.m_commandSinkForTest = writer; }
    template<typename Model>
    static bool send(Model& model, const std::shared_ptr<SetupStatusLifetime>& identity)
    { return model.sendSetupEntityCommand(QStringLiteral("xvtr set 2 rx_only=1"), {}, identity); }
    template<typename Model> static void reset(Model& model) { model.resetSetupObservations(); }
    template<typename Model> static void refreshLicense(Model& model, const QDateTime& now)
    { model.refreshLicenseSubscription(now); }
    template<typename Transport> static void feed(Transport& transport, const QString& line)
    { transport.processLine(line); }
    template<typename Transport> static void begin(Transport& transport)
    { transport.m_setupStatusSource.begin(); }
};

inline void applyCableStatus(UsbCableModel& model, const QString& serial, const QMap<QString, QString>& fields)
{
    const auto observation = decodeFlexSetupStatus(QStringLiteral("usb_cable ") + serial, fields,
                                                    std::make_shared<SetupStatusLifetime>());
    if (observation) { model.applyObservation(observation->usbCable); }
}
} // namespace AetherSDR
