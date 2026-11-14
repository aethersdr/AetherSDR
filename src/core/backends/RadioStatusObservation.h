#pragma once

#include "RadioDelta.h"
#include <QMetaType>
#include <optional>

namespace AetherSDR {

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

// One Flex "radio", "radio oscillator", "radio static_net_params" or
// "client 0x…" report, stamped with the status session it arrived in.
struct RadioStatusObservation {
    enum class Kind { Radio, Client, Oscillator, StaticNetwork };
    Kind kind{Kind::Radio};
    RadioDelta radio;
    ClientStatusValues client;
    OscillatorStatusValues oscillator;
    StaticNetworkValues network;
    quint64 statusEpoch{0};
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::RadioStatusObservation)
