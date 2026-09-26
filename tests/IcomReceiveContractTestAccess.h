#pragma once

#include "core/backends/icom/IcomCivBackend.h"
#include "core/backends/icom/IcomSession.h"

#include <algorithm>
#include <memory>

namespace AetherSDR::icom {

// One definition shared by the receive-contract and capacity test executables;
// reuse the existing friend instead of adding a production test hook.
struct IcomCivBackendTestAccess {
    static void selectModel(IcomCivBackend& backend, const IcomModel& model)
    {
        backend.m_model = &model;
        backend.publishCapabilities();
    }

    static void prepare(IcomCivBackend& backend)
    {
        // Unstarted IcomSession constructs no streams or sockets. Exercise
        // the production scheduler; the unconnected transport drops bytes.
        // The trace proves dispatch, not successful radio delivery.
        backend.m_session = std::make_unique<IcomSession>();
        backend.m_model = modelForId(0xA4);
        backend.m_connected = true;
        backend.m_sessionGeneration = 1;
        backend.m_mode = CivMode::Usb;
        backend.m_frequencyHz = 14'100'000;
    }

    static QString firstDispatched(const IcomCivBackend& backend)
    {
        // Read the first actual dispatch, not the most recent frame: a slow
        // test may already have dispatched a confirmation or a PBT command.
        for (const IcomCivBackend::CivTraceEntry& entry : backend.m_civTrace) {
            if (entry.outbound) {
                return entry.hex;
            }
        }
        return {};
    }

    static std::uint64_t dispatchCount(const IcomCivBackend& backend)
    {
        return backend.m_civScheduler.stats().dispatched;
    }

    static std::uint64_t queuedCount(const IcomCivBackend& backend)
    {
        return backend.m_civScheduler.stats().queued;
    }

    static void pump(IcomCivBackend& backend)
    {
        // sendUserCommand can enqueue after its captured pump timestamp.
        // Drive the next tick, but do not assume it is the first dispatch.
        backend.pumpCiv(backend.nowMs());
    }

    static void expireReply(IcomCivBackend& backend)
    {
        // Exercise the slow-run case deterministically, without a sleep or
        // event loop. This fixture is discarded after its future clock tick.
        const qint64 last = backend.m_civScheduler.stats().lastDispatchMs;
        backend.pumpCiv(std::max(backend.nowMs(), last)
                        + IcomCivScheduler::kReadTimeoutMs + 1);
    }

    static void observe(IcomCivBackend& backend, CivFrame frame, std::uint64_t generation = 1)
    {
        frame.to = kControllerAddress;
        frame.from = 0xA4;
        backend.onCivFrame(frame, generation);
    }
};

} // namespace AetherSDR::icom
