#pragma once

#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/FlexRadioStatus.h"
#include "core/backends/flex/FlexStatusIngress.h"
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
    static void apply(RadioModel& model, RadioStatusObservation observation)
    {
        // Without an ingress the session opens at connect, which a fixture skips.
        if (!model.m_observationSessionOpen) { model.beginStatusSession(); }
        observation.statusEpoch = model.m_observationEpoch;
        model.applyRadioObservation(observation);
    }
    static bool hasIngress(const RadioModel& model) { return bool(model.m_statusIngress); }
    // A connect's fresh status session, made the connected one.
    static quint64 beginConnect(RadioModel& model)
    {
        const quint64 epoch = model.beginStatusSession();
        model.m_connectedStatusEpoch = epoch;
        return epoch;
    }
    static void disconnected(RadioModel& model) { model.onDisconnected(); }
};

// A status line through the backend's own ingress, as the radio's arrives.
inline void observeRadioStatus(FlexBackend& backend, const QString& object, const QMap<QString, QString>& fields)
{
    QStringList pairs;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) { pairs << it.key() + u'=' + it.value(); }
    ParsedMessage line = CommandParser::parseLine(QStringLiteral("S0|") + object
        + (pairs.isEmpty() ? QString() : u' ' + pairs.join(u' ')));
    line.object = object;
    line.kvs = fields;
    backend.statusIngress()->deliverForTest(line);
}
inline void observeRadioStatus(FlexBackend& backend, const QMap<QString, QString>& fields)
{
    observeRadioStatus(backend, QStringLiteral("radio"), fields);
}
inline void observeRadioStatus(RadioModel& model, const QString& object, const QMap<QString, QString>& fields)
{
    if (FlexRadioStatusTestPeer::hasIngress(model)) {
        model.handleStatusForTest(object, fields);
    } else if (const auto observation = decodeFlexRadioStatus(object, fields)) {
        FlexRadioStatusTestPeer::apply(model, *observation);
    }
}
} // namespace AetherSDR
