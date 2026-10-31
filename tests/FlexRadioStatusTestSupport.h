#pragma once

#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/FlexRadioStatus.h"
#include "core/backends/flex/RadioConnection.h"
#include "core/backends/flex/WanConnection.h"
#include "models/RadioModel.h"

namespace AetherSDR {
class FlexRadioStatusTestPeer {
public:
    static void foreign(RadioModel& model, int id, quint32 owner) { model.m_foreignSliceOwners[id] = owner; }
    static quint32 foreign(const RadioModel& model, int id) { return model.m_foreignSliceOwners.value(id); }
    static quint32 localHandle(const RadioModel& model) { return model.clientHandle(); }
    static bool intentional(const RadioModel& model) { return model.m_intentionalDisconnect; }
    static void reset(RadioModel& model) { model.resetRadioObservations(); }
    static quint64 generation(const RadioModel& model) { return model.m_sessionGeneration; }
    static void replaceSession(RadioModel& model) { ++model.m_sessionGeneration; }
    static void handle(RadioConnection& connection, quint32 handle) { connection.m_handle = handle; }
    static void line(RadioConnection& connection, const QString& line) { connection.processLine(line); }
    static void line(WanConnection& connection, const QString& line) { connection.processLine(line); }
    static void end(RadioConnection& connection) { connection.resetSessionState(); }
    static void end(WanConnection& connection) { connection.resetSessionState(); }
    static void begin(RadioConnection& connection) { connection.m_radioStatusSource.begin(); }
    static void begin(WanConnection& connection) { connection.m_radioStatusSource.begin(); }
    static void awaitingCertificate(WanConnection& connection, bool value) { connection.m_awaitingCertDecision = value; }
};

inline void observeRadioStatus(IRadioBackend& backend, const QString& object,
                              const QMap<QString, QString>& fields, quint32 localHandle = 0)
{
    const auto observation = decodeFlexRadioStatus(object, fields,
        std::make_shared<RadioStatusLifetime>(), localHandle);
    if (observation) { emit backend.radioStatusObserved(*observation); }
}
inline void observeRadioStatus(FlexBackend& backend, const QMap<QString, QString>& fields)
{
    const auto observation = decodeFlexRadioStatus(QStringLiteral("radio"), fields,
        std::make_shared<RadioStatusLifetime>());
    if (observation) { backend.acceptRadioStatus(*observation); }
}
inline void observeRadioStatus(RadioModel& model, const QString& object,
                              const QMap<QString, QString>& fields)
{
    observeRadioStatus(*model.backend(), object, fields, FlexRadioStatusTestPeer::localHandle(model));
}
} // namespace AetherSDR
