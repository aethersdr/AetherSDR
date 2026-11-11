#include "AdditionalRadioBackends.h"
#include "kenwood/KenwoodBackend.h"

namespace AetherSDR {
std::unique_ptr<IRadioBackend> makeAdditionalRadioBackend(const QString& family)
{
    if (family.compare(QLatin1String("kenwood"), Qt::CaseInsensitive) == 0) {
        return std::make_unique<kenwood::KenwoodBackend>();
    }
    return nullptr;
}
}
