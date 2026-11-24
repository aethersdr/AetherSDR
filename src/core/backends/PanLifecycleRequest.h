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
    // Closing the pan that hosts the TX slice while the radio is transmitting.
    // Refused: the backend refuses it (a close would hand transmit to another
    // receiver mid-over), so RadioModel refuses it up front, with the same
    // Notice, before anything above the seam starts tearing the pan down.
    enum class TxPanCloseWhileKeyed { Allowed, Refused };
    TxPanCloseWhileKeyed txPanCloseWhileKeyed{TxPanCloseWhileKeyed::Allowed};
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
