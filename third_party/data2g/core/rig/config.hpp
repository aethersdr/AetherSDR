// The rig's settings as plain types: HamlibConfig and its enums, the model
// numbers, and each enum's spelling in the options and saved settings
// (SSTVAE's lowercase ones). Part of data2g_core and needs no libhamlib, so
// app/ and gui/ can build and check a configuration in any build; the
// backend that uses one is rig/hamlib/hamlib.hpp (data2g_rig).
#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace data2g::rig {

inline constexpr int MODEL_DUMMY = 1;
inline constexpr int MODEL_NET_RIGCTL = 2;

// What the model's rig_pathname names, so a settings page shows only the
// fields that mean something: a serial device and its line settings, a
// host:port, some other device path, or nothing (the dummy).
enum class PortType { None, Serial, Network, Other };

struct RigModel {
    int model = 0;
    std::string manufacturer, name, version;
    std::string status;  // Hamlib's backend status: Alpha, Beta, Stable...
    PortType port = PortType::Other;

    // "Elecraft K4 (Stable)": what a picker shows.
    std::string label() const {
        std::string out = manufacturer;
        if (!out.empty() && !name.empty()) out += " ";
        out += name;
        if (!status.empty()) out += " (" + status + ")";
        return out;
    }
};

// Vox: do not key at all (the rig keys on its own audio).
enum class PttMethod { Vox, Cat, Dtr, Rts };
// Which input CAT keying selects on a rig with two (RIG_PTT_RIG_MICDATA).
enum class PttAudio { Mic, Data };
// Serial line settings. Default: leave the token unset, the backend's own.
enum class DataBits { Default, Seven, Eight };
enum class StopBits { Default, One, Two };
enum class Parity { Default, None, Odd, Even };
enum class Handshake { Default, None, XonXoff, Hardware };
// Held for the session: an interface powered from the control lines.
enum class LineState { Default, High, Low };
// Set once the rig is open; None leaves the operator's mode alone.
enum class RigMode { None, Usb, PktUsb };

// Each enum's names, in the order a settings page and a "choose from" list
// show them. The pairing is explicit: reordering an enum changes nothing.
template <typename E>
struct Named {
    E value;
    std::string_view name;
};
inline constexpr std::array DATA_BITS_NAMES{Named<DataBits>{DataBits::Default, "default"}, Named<DataBits>{DataBits::Seven, "7"},
                                            Named<DataBits>{DataBits::Eight, "8"}};
inline constexpr std::array STOP_BITS_NAMES{Named<StopBits>{StopBits::Default, "default"}, Named<StopBits>{StopBits::One, "1"},
                                            Named<StopBits>{StopBits::Two, "2"}};
inline constexpr std::array PARITY_NAMES{Named<Parity>{Parity::Default, "default"}, Named<Parity>{Parity::None, "none"},
                                         Named<Parity>{Parity::Odd, "odd"}, Named<Parity>{Parity::Even, "even"}};
inline constexpr std::array HANDSHAKE_NAMES{Named<Handshake>{Handshake::Default, "default"}, Named<Handshake>{Handshake::None, "none"},
                                            Named<Handshake>{Handshake::XonXoff, "xonxoff"},
                                            Named<Handshake>{Handshake::Hardware, "hardware"}};
inline constexpr std::array LINE_STATE_NAMES{Named<LineState>{LineState::Default, "default"}, Named<LineState>{LineState::High, "high"},
                                             Named<LineState>{LineState::Low, "low"}};
inline constexpr std::array PTT_METHOD_NAMES{Named<PttMethod>{PttMethod::Vox, "vox"}, Named<PttMethod>{PttMethod::Cat, "cat"},
                                             Named<PttMethod>{PttMethod::Dtr, "dtr"}, Named<PttMethod>{PttMethod::Rts, "rts"}};
inline constexpr std::array PTT_AUDIO_NAMES{Named<PttAudio>{PttAudio::Mic, "mic"}, Named<PttAudio>{PttAudio::Data, "data"}};
inline constexpr std::array RIG_MODE_NAMES{Named<RigMode>{RigMode::None, "none"}, Named<RigMode>{RigMode::Usb, "usb"},
                                           Named<RigMode>{RigMode::PktUsb, "pkt_usb"}};

// `name`'s value in `table`; nullopt: not one of its names.
template <typename E, std::size_t N>
constexpr std::optional<E> from_name(const std::array<Named<E>, N>& table, std::string_view name) {
    for (const auto& n : table)
        if (n.name == name) return n.value;
    return std::nullopt;
}

namespace detail {
template <typename E, std::size_t N>
constexpr bool distinct(const std::array<Named<E>, N>& table) {
    for (std::size_t i = 0; i < N; ++i)
        for (std::size_t j = i + 1; j < N; ++j)
            if (table[i].value == table[j].value || table[i].name == table[j].name) return false;
    return true;
}
}  // namespace detail
static_assert(detail::distinct(DATA_BITS_NAMES) && detail::distinct(STOP_BITS_NAMES) && detail::distinct(PARITY_NAMES) &&
              detail::distinct(HANDSHAKE_NAMES) && detail::distinct(LINE_STATE_NAMES) && detail::distinct(PTT_METHOD_NAMES) &&
              detail::distinct(PTT_AUDIO_NAMES) && detail::distinct(RIG_MODE_NAMES));

struct HamlibConfig {
    int model = MODEL_NET_RIGCTL;
    // Serial device, or "host:port" for MODEL_NET_RIGCTL. Empty: Hamlib's default.
    std::string device = "localhost:4532";
    int baud = 0;  // 0: the backend's default
    DataBits data_bits = DataBits::Default;
    StopBits stop_bits = StopBits::Default;
    Parity parity = Parity::Default;
    Handshake handshake = Handshake::Default;
    LineState dtr = LineState::Default;
    LineState rts = LineState::Default;

    PttMethod ptt_method = PttMethod::Cat;
    PttAudio ptt_audio = PttAudio::Mic;
    std::string ptt_device;  // DTR/RTS keying on another port; empty: the CAT device

    RigMode mode = RigMode::None;

    // One timeout and one retry, both ours: RigController::stop() abandons a
    // worker that only exits when its call gives up.
    int timeout_ms = 1000;
    int retries = 1;
};

}  // namespace data2g::rig
