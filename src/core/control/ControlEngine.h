#pragma once

#include "ControlResourceStore.h"
#include "ControlService.h"

namespace AetherSDR::control {

// The control plane a daemon serves: one resource store and one service, so
// every listener (the local socket, the network listener) serves sessions of
// the same engine. That gives all of them one resource store, one radio
// generation and one TX client registry. Owned by the process; the listeners
// hold references and must not outlive it.
struct ControlEngine {
    explicit ControlEngine(RadioConnectionTarget* connectionTarget = nullptr)
        : service(&resources, connectionTarget) {}
    ControlEngine(const ControlEngine&) = delete;
    ControlEngine& operator=(const ControlEngine&) = delete;

    ControlResourceStore resources;
    ControlService service;
    // Live sessions across every network listener (TLS and tailnet together),
    // so the network budget is one budget. The local socket counts apart.
    int networkSessions{0};
};

} // namespace AetherSDR::control
