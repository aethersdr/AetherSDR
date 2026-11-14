// data2g/host.py's Host: VARA command semantics over an arq::Engine. No
// sockets, no audio (those are the app's: app/station.cpp runs the loop,
// app/tcp_servers.hpp the command, data and KISS ports). command()
// and data_in() take the client's side; after_step() collects what goes
// back after each block. Everything here touches the engine's session
// stage, so in worker mode it runs only through Engine::post() and the
// after_block callback.
//
// Commands: MYCALL call..., LISTEN ON|OFF|CQ, ACCEPT [host]:port (LISTEN ON,
// each session's data over a connection made to host:port, default
// the command client's own: host.py), CONNECT from to, DISCONNECT,
// ABORT, CQFRAME call bw, BW500|BW1200|BW2300|BW2750, CHAT ON|OFF, VERSION,
// SN (-> SN snr, the session peer's last burst decoded; WRONG before one),
// CLEANTXBUFFER (host.py's Host._clean), DATA2G ON|OFF (a client that knows
// Data2G: plain VARA BUFFER, CLEANTXBUFFER reports host byte offsets, data
// before CONNECTED dropped; off again when the client goes), and IGNORED's
// settings; broadcast (docs/broadcast.md): BCAST OPEN group
// [FROM call], BCAST CLOSE n, BCAST MODE n [AUTO] [DUP] mode, MODES. Replies OK /
// WRONG (MYCALL's OK followed by REGISTERED call for each call). Notifications: CONNECTED src dst bw, DISCONNECTED, PTT ON|OFF, BUSY
// ON|OFF, BUFFER n, IAMALIVE, MODE; broadcast statuses (BCAST ...) once the
// client has sent a BCAST command.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "arq/engine.hpp"

namespace data2g::host {

inline constexpr std::string_view VERSION = "Data2G " DATA2G_VERSION;  // data2g.__version__ in host.py
inline constexpr double ALIVE_S = 60.0;          // IAMALIVE on the command port this often, as VARA does
inline constexpr double BUFFER_REPEAT_S = 30.0;  // a nonzero BUFFER is repeated this often
inline constexpr double VOLUME_MIN_DB = -60.0, VOLUME_MAX_DB = 20.0;  // TXPOWER's range: the GUI's output volume
inline constexpr double DIAL_TIMEOUT_S = 1.0;    // ACCEPT's connection must be up by then (the caller waits ~3.5 s)

struct Addr {
    std::string host;
    int port;
};
// ACCEPT's [host]:port, or nullopt.
std::optional<Addr> parse_addr(std::string_view s, std::string_view default_host = "127.0.0.1");

// VARA settings a client may send that have no Data2G meaning (yet): OK, logged.
bool ignored(std::string_view cmd);
// BW500 -> 0, BW1200 -> 1, BW2300/BW2750 -> 2; nullopt: not a BW command.
std::optional<int> bw_cap(std::string_view cmd);
// MODES (and --list-modes): one line per mode within the cap, narrowest
// first: MODE name bandwidth-Hz bytes-per-codeword max-codewords
// seconds-at-1 seconds-at-max.
std::vector<std::string> mode_lines(int cap);

class Host {
public:
    // `buffer_credit`: queued bytes BUFFER may leave out, at most (nullopt:
    // the next burst's whole capacity; 0: plain VARA, all of them).
    explicit Host(arq::Engine& engine, std::optional<int> buffer_credit = std::nullopt);
    virtual ~Host() = default;

    // `peer`: the address the command came from (ACCEPT's default host).
    void command(std::string_view line, std::string_view peer = "127.0.0.1");
    // The command client's TCP connection closed: it owned the session.
    void client_gone();
    // The operator's Abort: a session under way (connecting, connected or
    // disconnecting) is dropped at once, as ABORT does, and the client told
    // DISCONNECTED. False, and nothing sent, when there was none.
    bool abort_session();
    void data_in(arq::ByteView data);
    void after_step(bool ptt);

    arq::Engine& engine;
    int cap = 2;
    bool listening = false;
    std::optional<Addr> accept_addr;  // ACCEPT host:port
    // the owner's: true once a connection to the address carries the session's data
    std::function<bool(const Addr&)> dial;
    // false: the owner sends PTT ON/OFF itself, when the rig is keyed and released (the engine's PTT runs ahead of both)
    bool announce_ptt = true;
    std::optional<int> buffer_credit;
    bool data2g = false;               // DATA2G ON
    double output_volume = 0.0;        // dB, TXPOWER: the owner applies it
    std::vector<std::string> out_cmd;  // lines for the command port, without CR
    arq::Bytes out_data;               // bytes for the data port

protected:
    // What BUFFER counts. Seams for a binding whose session or policy is a
    // Python object (tests patch them).
    struct Queued {
        std::int64_t unsent = 0, unacked = 0;
        bool station = false;
    };
    // `vara`: Session::queued_bytes's
    virtual Queued queued(bool vara);
    // The policy's next_capacity(station); nullopt when it has none.
    virtual std::optional<std::int64_t> next_capacity();

private:
    // BCAST OPEN group [FROM call] | CLOSE n | MODE n [AUTO] [DUP] mode -> the reply, nullopt for WRONG.
    std::optional<std::string> bcast_command(const std::vector<std::string>& args);
    bool accept_connect(const std::string& caller, const std::string& callee);
    std::string clean(std::optional<std::int64_t> upto);  // CLEANTXBUFFER [n] -> the reply
    void session_over();  // the session is ending: an unsettled limbo goes as SETTLED from split to UNKNOWN

    bool bcast_ = false;  // the client has sent BCAST: broadcast statuses go to it
    bool up_ = false;     // CONNECTED sent, DISCONNECTED not yet (DATA2G ON: data before it is dropped)
    bool ptt_ = false, busy_ = false;
    std::optional<std::int64_t> buffer_ = 0;  // nullopt: answer the next step whatever it is
    double buffer_t_ = 0.0;                   // engine time of the last BUFFER line
    std::optional<std::string> mode_;
    double alive_ = 0.0;                      // engine time of the last IAMALIVE
};

}  // namespace data2g::host
