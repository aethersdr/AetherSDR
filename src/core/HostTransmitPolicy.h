#pragma once

namespace AetherSDR {

// Whether this process has a transmit path for radios whose backend keys from
// the host (the HL2). The desktop does (the default); aetherd does not (no such
// TX grant exists), so it sets false before any backend is built, and those
// backends declare themselves receive only. Backends read it at construction.
void setHostOffersTransmit(bool offers) noexcept;
[[nodiscard]] bool hostOffersTransmit() noexcept;

}  // namespace AetherSDR
