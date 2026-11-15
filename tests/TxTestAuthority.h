#pragma once

#include "core/TxCoordinator.h"

// Socket-free tests opt into a real engine-local grant. No default context or
// transport test hook can manufacture this authority for production callers.
struct TxTestAuthority {
    AetherSDR::TxCoordinator coordinator{[](const auto&, auto) {}};
    AetherSDR::TxCoordinator::Actor actor = coordinator.registerActor({true, 0});
    AetherSDR::TxCoordinator::Producer producer = coordinator.registerProducer();
    AetherSDR::TxCoordinator::Operation operation =
        coordinator.acquire(actor, AetherSDR::TxCoordinator::monotonicMs()).operation;
    AetherSDR::TxCoordinator::Context context = coordinator.mediaContext(producer, operation);
};
