#pragma once

// Health that survives disconnection — the seam verb `IRadioBackend` cannot
// provide, and why it is a seam question rather than a family one.
//
// THE HOLE. `RadioModel::backendHealthSnapshot()` is
// `m_backend ? m_backend->healthSnapshot() : HealthSnapshot{}`, and `m_backend`
// is constructed inside `connectToRadio()`. So every health reading this
// application can take is conditional on a connection existing. For most
// questions that is correct — a radio you are not talking to has nothing to
// say. For some it is exactly backwards: "is anyone else using this radio",
// "is it powered and reachable", "what is its PA temperature while somebody
// else holds the stream" are questions whose whole point is that we are NOT
// connected.
//
// WHY A CAPABILITY FLAG CANNOT ANSWER IT. `RadioCapabilities` is produced by a
// connected backend. In the two states this interface exists for — nothing
// connected, and another client holding the radio — there is no backend, and
// therefore no capability record to read. A capability gate is the right shape
// for "this radio cannot do X" and the wrong shape for "there is no radio
// object yet". That is the gap; this is the seam that fills it.
//
// WHAT THIS IS NOT. It is not a second health path for connected radios. While
// a backend exists, `healthSnapshot()` remains authoritative and wins every key
// collision — an in-band reading arrives on our own cadence and an out-of-band
// probe does not. This fills the gaps and owns the rows that say which path
// spoke.
//
// NO FAMILY NAME APPEARS ABOVE THE SEAM. A family that has an offline
// instrument DECLARES one from its own directory, under its own family key, via
// OfflineHealthRegistry::declare(). Shared code asks the registry whether the
// selected family declared anything and never names one. That is the rule in
// `docs/HERMES.md`, "For coding agents — keep bring-up inside the family
// backend": a family string test above the seam is forbidden outright, and the
// replacement it asks for is a declaration with a consumer that already exists.
// The consumer is `RadioModel`, below.

#include "core/backends/IRadioBackend.h"   // HealthSnapshot

#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

namespace AetherSDR {

// An instrument whose lifetime is the MODEL's, not a connection's.
//
// Implementations live under `src/core/backends/<family>/`. Nothing here knows
// how one talks to a radio, and nothing here is allowed to: this interface
// carries no wire concept beyond an address, because the moment it carries two
// it has started to describe one family's protocol.
class IOfflineHealthSource {
public:
    virtual ~IOfflineHealthSource() = default;

    // Aim it at a radio. A null address means STOP AND FORGET, and the second
    // half is not optional: a reading belongs to the radio it came from, so
    // carrying the old radio's values into the new one's rows is a frozen
    // reading wearing a fresh address.
    //
    // Must never connect, never write, and never take a session. The caller's
    // premise is that somebody else may be using this radio.
    virtual void setOfflineTarget(const QHostAddress& addr) = 0;

    // Whether an address is currently aimed. "Not aimed" and "aimed but silent"
    // are different states and a consumer has to be able to tell them apart.
    [[nodiscard]] virtual bool hasOfflineTarget() const = 0;

    // Somebody is watching. Implementations may use this to back off when
    // nobody is, which is the difference between an instrument and a beacon.
    virtual void noteOfflineDemand() = 0;

    // Rows for the health snapshot. Always answers — backend or no backend —
    // which is the entire reason this interface exists.
    [[nodiscard]] virtual IRadioBackend::HealthSnapshot offlineHealthRows() const = 0;
};

// Family → offline-source factory.
//
// A family registers itself from its own translation unit. Shared code asks
// `declaredFor()` and `create()` and never writes a family name.
//
// LINKAGE, because this is the failure mode of every self-registration scheme
// and it is silent: a registrar in a translation unit that nothing references
// can be dropped by the linker from a static archive, and the feature then
// simply does not exist with no error anywhere. The HL2 registrar lives in
// `Hl2TelemetryService.cpp`, which is reached because `Hl2Backend.cpp`
// references the type, which is in turn reached because `RadioModel::makeBackend`
// names `hl2::Hl2Backend`. `offline_health_registry_test` asserts the
// declaration is actually present rather than assuming the chain holds.
class OfflineHealthRegistry {
public:
    using Factory =
        std::function<std::unique_ptr<IOfflineHealthSource>(QObject* parent)>;

    // Declare that `family` has an offline health source. Last declaration
    // wins; declaring twice is a programming error rather than a merge.
    static void declare(const QString& family, Factory make);

    // Did this family declare one? The gate shared code asks INSTEAD of
    // comparing a family string. False for every family that did not, which is
    // most of them, and that is the honest answer rather than a refusal.
    [[nodiscard]] static bool declaredFor(const QString& family);

    // Build one, or null when the family declared nothing.
    [[nodiscard]] static std::unique_ptr<IOfflineHealthSource>
    create(const QString& family, QObject* parent);

private:
    static QHash<QString, Factory>& table();
};

}  // namespace AetherSDR
