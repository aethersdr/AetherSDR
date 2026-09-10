#pragma once

#include <QString>

namespace AetherSDR {

// Whether a discovered radio that is ALREADY IN USE by another client may
// still be joined by this one.
//
// Flex is the only family that shares: multiFLEX gives every GUI client its
// own session, slices and streams. Every other family served today is
// single-client at the protocol layer — HPSDR Protocol 1 has one host slot
// (#4448: connecting to a streaming HL2 wedges BOTH clients), and an Icom
// RS-BA1 session is exclusive — so a busy radio fails closed.
//
// Unknown or empty families fail closed too: refusing a share the radio
// could have taken is a retry; wedging someone's active QSO is not.
//
// This predicate exists so the rule has ONE home (it was previously
// duplicated in ConnectionPanel and MainWindow_Session, drifting apart is
// how #4448 regresses). It is family-keyed rather than capability-keyed
// only because it runs at DISCOVERY time, before any backend exists to ask
// for RadioCapabilities::hasMultiClientSessions — the M5 per-family static
// descriptors (#5262) replace this with that capability; do not grow this
// file into a second capability system in the meantime.
inline bool familySupportsSharedInUseConnect(const QString& family)
{
    return family.compare(QLatin1String("flex"), Qt::CaseInsensitive) == 0;
}

} // namespace AetherSDR
