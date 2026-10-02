#pragma once

// Stream-free HL2 telemetry, owned above the backend: an instrument for the
// no-connection case must not be owned by the connection (the backend exists
// only inside connectToRadio()). Application lifetime; owns the poller and
// answers health rows with or without a backend, whose in-band rows take
// precedence at the merge point.

#include "core/backends/IRadioBackend.h"        // HealthSnapshot
#include "core/backends/OfflineHealthSource.h" // IOfflineHealthSource, the seam it implements
#include "core/backends/hl2/Hl2TelemetryCadence.h"
#include "core/backends/hl2/MetisProtocol.h"    // DiscoveryReply

#include <QElapsedTimer>
#include <QHostAddress>
#include <QObject>
#include <QString>

#include <memory>

namespace AetherSDR::hl2 {

class Hl2TelemetryPoller;

// Implements IOfflineHealthSource, which is how anything above the seam reaches
// it. Nothing in src/models or src/core names this class or this family: the
// model holds the interface, and `Hl2TelemetryService.cpp` declares "hl2" to
// the registry from down here where the name belongs.
class Hl2TelemetryService : public QObject, public IOfflineHealthSource {
    Q_OBJECT

public:
    explicit Hl2TelemetryService(QObject* parent = nullptr);
    ~Hl2TelemetryService() override;

    // The radio to read. A null address stops the poller unless the broadcast
    // fallback is on (default off). Any target change drops the last reading,
    // which belongs to the radio it came from.
    void setTarget(const QHostAddress& addr);
    // Opt in to broadcasting when no target is set (default off; see
    // Hl2TelemetryPoller::setTarget).
    void setAllowBroadcastFallback(bool allow);

    // What the IQ path is doing. Driven by whoever knows: the backend while one
    // exists, the model's connection state otherwise. The cadence rule turns
    // this into an interval; this class does not restate it.
    void setLinkState(Hl2LinkState state);
    // How many times a link state has been pushed in, so a test can assert
    // that something drives setLinkState() periodically.
    [[nodiscard]] int linkStateUpdateCount() const noexcept;

    // Reading the health snapshot IS the demand signal — it is the one thing
    // every consumer does, so no consumer can forget to announce itself. Call
    // from the health path, not from a UI show/hide, or the bridge and the
    // dialog disagree about whether anyone is watching.
    void noteDemand();

    // Stream-free rows for the health snapshot. Always answers, backend or no
    // backend; that is the entire point of the class.
    [[nodiscard]] IRadioBackend::HealthSnapshot healthRows() const;

    // The newest stream-free reply, for the backend's in-band merge. nullopt
    // until one has arrived — never a default-constructed reply, because "the
    // radio has not answered" and "the radio answered with zeros" are different
    // claims and only one is a measurement.
    [[nodiscard]] std::optional<DiscoveryReply> lastReply() const;

    // ---- IOfflineHealthSource ----
    //
    // Thin forwarders on purpose. The interface is the vocabulary shared code
    // is allowed to use; these names are this family's. Keeping both means a
    // later change to what "demand" means here cannot silently redefine the
    // seam, and the HL2 tests go on driving the HL2 names.
    void setOfflineTarget(const QHostAddress& addr) override { setTarget(addr); }
    [[nodiscard]] bool hasOfflineTarget() const override;
    void noteOfflineDemand() override { noteDemand(); }
    [[nodiscard]] IRadioBackend::HealthSnapshot offlineHealthRows() const override
    {
        return healthRows();
    }

private:
    // Test access to the cached reply, which otherwise only the poller's signal
    // can set. Two of this class's rows are conditional on a reply having
    // arrived -- radioInUse among them, and it is the one whose correctness
    // depends on WHICH session the reply describes -- so a test that cannot
    // place one cannot reach them at all. The friend struct is the idiom
    // Hl2Backend already uses (Hl2DspReadbackTestAccess), and it adds no public
    // surface: a caller outside the test cannot name it.
    friend struct Hl2TelemetryServiceTestAccess;

    // Pimpl by unique_ptr, not a raw owning pointer: the destructor is the only
    // thing that has to see the complete type, and it is out of line below for
    // exactly that reason.
    struct Impl;
    std::unique_ptr<Impl> d;
};

// See the friend declaration above. Declared here and defined in the .cpp,
// where Impl is complete; nothing in production names it.
struct Hl2TelemetryServiceTestAccess {
    static void placeReply(Hl2TelemetryService& svc, const DiscoveryReply& r);
};

}  // namespace AetherSDR::hl2
