#pragma once

#include <QString>

namespace AetherSDR {

// Whether a discovered radio ALREADY IN USE by another client may still be joined.
// Only Flex shares (multiFLEX gives each client its own session). HPSDR Protocol 1
// has one host slot (joining a streaming HL2 wedges both clients, #4448) and an
// Icom RS-BA1 session is exclusive, so busy radios and unknown families fail
// closed. Family-keyed only because it runs at discovery, before a backend can
// report hasMultiClientSessions; the #5262 M5 per-family descriptors replace it.
// Do not grow this file into a second capability system meanwhile.
inline bool familySupportsSharedInUseConnect(const QString& family)
{
    return family.compare(QLatin1String("flex"), Qt::CaseInsensitive) == 0;
}

} // namespace AetherSDR
