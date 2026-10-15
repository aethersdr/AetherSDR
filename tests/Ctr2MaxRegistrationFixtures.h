// Synthetic CTR2-Max registration volumes (LittleFS, 16 blocks of 4096 bytes),
// made with littlefs-python: one registered to the placeholder call N0CALL,
// one with settings but no Reg.txt. qCompress()ed and base64-encoded.
#pragma once

namespace Ctr2MaxRegistrationFixtures {
inline constexpr const char* kRegistered =
    "AAEAAHja7dehSkNRHAbw/5w4kI07g9XdJ9AhGA1iHQsG+4SriFcceKcGYc9hN1rMVp/CIIJFsJg0DHbdRHwERfb7"
    "cU75TvvCB2chIt6S8iM/LIo82z9de4pmJeYiml8nyukty+G4GpFGfWm3lw+y1eKiSCOqZyf54Djb3GgvpjHf2MkO"
    "vh+Sbnt7q9NpTOJ6Mo33ekeTuNZvjcYvN+vLJTCDJmsSz+ndoybAAgAztwDnl/cPTU3AH6v94v//J+63Ru+v11e3"
    "6ge/AMACADO0AMNup7KiCQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAACAf+wT2s0/lA==";
inline constexpr const char* kUnregistered =
    "AAEAAHja7dc9SgMBEAbQ0Y1ggmA8QbYUizUJWFik8AoWVhYGEmFhZQNuJI3YeAPxDN7AJncRwQsIFhaBrD94BUHM"
    "e8xU033FB9OIiNft+r3Iq6oYn1/uP0d7LdYj2t8T9dfW9c0yiUhjc+dkWEzHWTWr0ojkqiymF+PBQbeVxlZyPBzl"
    "5c+pOekslru3D7MaWEGfxREv6fxJEqABgJVrgLP5UfNOEvAXbfzO/9/IJ4PeYT/rZv2s15p0Fm/l7Ppe3OAVADQA"
    "sEINUO6dPiaSAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAD4"
    "Rz4AHPxc0A==";
} // namespace Ctr2MaxRegistrationFixtures
