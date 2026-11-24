#pragma once

#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/FlexSliceStatus.h"
#include "core/backends/flex/FlexStatusIngress.h"
#include "models/RadioModel.h"

namespace AetherSDR {

class FlexSliceStatusTestPeer {
public:
    // Through the backend's status ingress when the model has one, as a radio
    // line arrives; otherwise decoded here and stamped with the model's session.
    static void apply(RadioModel& model, int id, QMap<QString, QString> fields, bool removed)
    {
        if (removed) { fields.insert(QStringLiteral("in_use"), QStringLiteral("0")); }
        if (model.m_statusIngress) {
            model.handleStatusForTest(QStringLiteral("slice %1").arg(id), fields);
            return;
        }
        auto observation = FlexSliceStatus::decode(QStringLiteral("slice %1").arg(id), fields);
        if (!observation) { return; }
        // Without an ingress the session opens at connect, which a fixture skips.
        if (!model.m_observationSessionOpen) { model.beginStatusSession(); }
        observation->statusEpoch = model.m_observationEpoch;
        model.applySliceObservation(*observation);
    }
    static void beginConnect(RadioModel& model) { model.beginStatusSession(); }
};

// The status session a (re)connect begins, which a fixture that stages
// reconnect models without connecting has to begin itself.
inline void beginFlexStatusSession(RadioModel& model) { FlexSliceStatusTestPeer::beginConnect(model); }

inline void applyFlexSliceStatus(RadioModel& model, int id,
                                const QMap<QString, QString>& fields, bool removed = false)
{
    FlexSliceStatusTestPeer::apply(model, id, fields, removed);
}

// A "slice N" line through the backend's own ingress.
inline void publishFlexSliceStatus(FlexBackend& backend, int id,
                                  const QMap<QString, QString>& fields)
{
    QStringList pairs;
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) { pairs << it.key() + u'=' + it.value(); }
    ParsedMessage line = CommandParser::parseLine(QStringLiteral("S0|slice %1 ").arg(id) + pairs.join(u' '));
    line.object = QStringLiteral("slice %1").arg(id);
    line.kvs = fields;
    backend.statusIngress()->deliverForTest(line);
}

} // namespace AetherSDR
