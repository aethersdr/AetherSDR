#pragma once

#include "RadioControlTypes.h"

#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QString>
#include <cmath>
#include <functional>
#include <optional>

namespace AetherSDR {

class RadioModel;

struct SliceCreateRequest {
    QString panId;
    double frequencyHz{0.0};
    std::optional<QString> mode;
    std::optional<QString> rxAntenna;
    bool requiresReceipt{false};

    bool valid() const
    {
        return !panId.isEmpty() && std::isfinite(frequencyHz) && frequencyHz > 0.0
            && (!mode || !mode->isEmpty()) && (!rxAntenna || !rxAntenna->isEmpty());
    }
};

// An acknowledged ID is not a published, owned slice. Status establishes that.
struct SliceCreateReply {
    RadioControlReply result;
    std::optional<int> sliceId;
};
using SliceCreateCompletion = std::function<void(const SliceCreateReply&)>;

// Minted only after a create reply joins its original session's owned model.
// A numeric ID alone never grants permission to reap a transient receiver.
class CreatedSliceReceipt {
public:
    int sliceId() const { return m_sliceId; }
    bool valid() const { return m_sliceId >= 0 && !m_identity.isNull(); }

private:
    friend class RadioModel;
    int m_sliceId{-1};
    QPointer<QObject> m_identity;
    RadioControlSession m_session{};
    quint64 m_retirement{0};
};

struct SliceCreationResult {
    RadioControlReply result;
    CreatedSliceReceipt created;
};
using SliceCreationCompletion = std::function<void(const SliceCreationResult&)>;

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::SliceCreateReply)
Q_DECLARE_METATYPE(AetherSDR::SliceCreateRequest)
Q_DECLARE_METATYPE(AetherSDR::CreatedSliceReceipt)
Q_DECLARE_METATYPE(AetherSDR::SliceCreationResult)
