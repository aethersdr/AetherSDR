#pragma once

#include <QString>
#include <memory>

namespace AetherSDR {
class IRadioBackend;
// Construction stays below the seam; models need no concrete vendor header.
std::unique_ptr<IRadioBackend> makeAdditionalRadioBackend(const QString& family);
}
