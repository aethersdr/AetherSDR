#pragma once

// Health that survives disconnection: answers questions whose point is that we
// are NOT connected ("is anyone else using this radio", "is it reachable", PA
// temperature while another client holds it). A backend's healthSnapshot() blanks
// when its link is not delivering, and RadioCapabilities needs a connected
// backend, so neither can answer these.
// While a backend exists, healthSnapshot() stays authoritative and wins every key
// collision; this fills gaps. No family name above the seam: a family declares
// its instrument via OfflineHealthRegistry::declare() (docs/HERMES.md, "keep
// bring-up inside the family backend"); RadioModel is the consumer.

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

// Family → offline-source factory. A family registers from its own translation
// unit; shared code asks declaredFor() / create() and never names a family.
// LINKAGE: a registrar in an unreferenced TU can be silently dropped from a static
// archive. The HL2 registrar in Hl2TelemetryService.cpp is reached via
// Hl2Backend.cpp ← RadioModel::makeBackend; offline_health_registry_test asserts
// the declaration is present.
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
