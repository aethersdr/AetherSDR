#include "core/AutomationIsolationPolicy.h"
#include "core/backends/sim/DemoRadioConstants.h"

namespace AetherSDR::AutomationIsolationPolicy {
bool allowsRadio(const QString& family, const QString& serial)
{
    return !demoOnly() || (family == DemoRadio::family() && serial == DemoRadio::serial());
}
} // namespace AetherSDR::AutomationIsolationPolicy
