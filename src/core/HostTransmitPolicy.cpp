#include "core/HostTransmitPolicy.h"

#include <atomic>

namespace AetherSDR {

namespace {
std::atomic<bool> g_hostOffersTransmit{true};
}

void setHostOffersTransmit(bool offers) noexcept
{
    g_hostOffersTransmit.store(offers);
}

bool hostOffersTransmit() noexcept
{
    return g_hostOffersTransmit.load();
}

}  // namespace AetherSDR
