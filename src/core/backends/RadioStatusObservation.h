#pragma once

#include "RadioDelta.h"
#include <QMetaType>
#include <atomic>
#include <memory>
#include <optional>

namespace AetherSDR {

// Ordinary reports expire at transport close. Terminal reports remain deliverable
// until the owner thread consumes the ordered end marker. A new session closes both.
struct RadioStatusLifetime {
    std::atomic_bool active{true};
    std::atomic_bool deliveryOpen{true};
};

struct ClientStatusValues {
    enum class Action { Connected, Updated, Disconnected };
    Action action{Action::Updated};
    quint32 handle{0};
    std::optional<QString> clientId;
    std::optional<QString> program;
    std::optional<QString> station;
    std::optional<QString> source;
    std::optional<bool> localPtt;
    bool forced{false};
    bool duplicateClientId{false};
    bool wanValidationFailed{false};
};

struct OscillatorStatusValues {
    std::optional<QString> state;
    std::optional<QString> setting;
    std::optional<bool> locked;
    std::optional<bool> externalPresent;
    std::optional<bool> gpsPresent;
    std::optional<bool> tcxoPresent;
};

struct StaticNetworkValues {
    std::optional<QString> ip;
    std::optional<QString> netmask;
    std::optional<QString> gateway;
};

struct RadioStatusObservation {
    enum class Kind { Radio, Client, Oscillator, StaticNetwork, SessionEnded };
    Kind kind{Kind::Radio};
    quint32 localHandle{0};
    RadioDelta radio;
    ClientStatusValues client;
    OscillatorStatusValues oscillator;
    StaticNetworkValues network;
    std::optional<bool> calibrationDone;
    std::shared_ptr<RadioStatusLifetime> lifetime;

    bool current() const
    {
        const bool terminal = kind == Kind::SessionEnded
            || (kind == Kind::Client && client.action == ClientStatusValues::Action::Disconnected);
        return lifetime && lifetime->deliveryOpen.load(std::memory_order_acquire)
            && (terminal || lifetime->active.load(std::memory_order_acquire));
    }
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::RadioStatusObservation)
