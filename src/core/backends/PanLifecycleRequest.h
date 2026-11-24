#pragma once

#include "RadioControlTypes.h"
#include <QMetaType>
#include <QSet>
#include <QString>
#include <functional>
#include <optional>

namespace AetherSDR {

enum class PanLifecycleAction { Create, Remove, RefreshInventory, RfGainInfo };
struct PanLifecycleControl {
    enum class Creation { Native, Answered };
    Creation creation{Creation::Native};
    QSet<PanLifecycleAction> actions;
    // A native create can wait for local work before publishing a pan or
    // definitively failing. It remains fenced to the original session.
    bool createWaitsForLocalBuild{false};
    bool supports(PanLifecycleAction action) const { return actions.contains(action); }
};
enum class PanCloseIntent { Ordinary, ExplicitTeardown };
struct PanLifecycleRequest {
    PanLifecycleAction action;
    QString panId;
    QString waterfallId;
};
struct PanRfGainRange { int low; int high; int step; };
struct PanLifecycleReply {
    RadioControlReply result;
    QString panId;
    std::optional<PanRfGainRange> rfGainRange;
};
using PanLifecycleCompletion = std::function<void(const PanLifecycleReply&)>;

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::PanLifecycleRequest)
