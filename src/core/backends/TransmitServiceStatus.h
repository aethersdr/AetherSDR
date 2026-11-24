#pragma once

#include <QMetaType>
#include <QString>
#include <optional>

namespace AetherSDR {

// A radio voice keyer report: its global state, one recording slot, or a
// slot's removal. Stamped with the status session it arrived in.
struct DvkStatusObservation {
    enum class Kind { Status, Recording, Deleted };
    enum class State { Unknown, Disabled, Idle, Recording, Preview, Playback };
    Kind kind{Kind::Status};
    State state{State::Unknown};     // Kind::Status
    int id{-1};                      // the active slot (Status) or the slot (Recording, Deleted)
    std::optional<bool> enabled;     // Kind::Status
    QString name;                    // Kind::Recording; empty when the radio named none
    int durationMs{0};               // Kind::Recording
    quint64 statusEpoch{0};
};

// The radio's NAVTEX state (FlexLib 4.2.18 NavtexStatus), case-insensitive on the wire.
enum class NavtexState { Inactive, Active, Transmitting, QueueFull, Unlicensed, Error };

// A NAVTEX report: the global state, or one message's transmit confirmation.
struct NavtexStatusObservation {
    enum class Kind { Status, Sent };
    Kind kind{Kind::Status};
    NavtexState state{NavtexState::Error};   // Kind::Status
    uint idx{0};                             // Kind::Sent
    uint serial{0};                          // Kind::Sent
    quint64 statusEpoch{0};
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::DvkStatusObservation)
Q_DECLARE_METATYPE(AetherSDR::NavtexStatusObservation)
