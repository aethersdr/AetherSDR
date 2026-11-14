// data2g/kisslink.py: broadcast over KISS (docs/broadcast.md). Named groups,
// each a KISS port (port 0 always open, on "KISS 0"); every codeword of a
// group's bursts is CRC-masked with the group's key, and the control names
// its group, so a receiver reads the control from the mask-free decode and
// checks it under that name's key. Rate shifting per port (off by default):
// a connected-mode AX.25 frame goes in its next hop's reported mode, anything
// else in the port's mode. The burst layout is in the Python module's
// docstring. monitor/ reads broadcast bursts through read_burst_control and
// read_burst_frames.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "arq/phy.hpp"
#include "arq/policy.hpp"

namespace data2g::kisslink {

using arq::Bytes;
using arq::ByteView;

inline constexpr int VERSION = 3;
inline constexpr int DUP = 0x08;  // the control header's dup bit: each control codeword sent twice, RV 0 then RV 1
inline constexpr int T_GROUP = 1, T_GROUP_FROM = 2, T_REPORTS = 3;  // control TLVs
inline constexpr std::string_view PORT0_GROUP = "KISS 0";
inline constexpr int N_PORTS = 16;  // KISS port numbers 0-15
inline constexpr double REPORT_MAX_S = 180.0;  // a report older than this is not followed
inline constexpr double HEARD_MAX_S = 600.0;   // stations reported on: heard this recently
inline constexpr double SLOT_S = 1.0;          // the shortest p-persistence slot
inline constexpr double MIN_SUCCESS = 0.9;     // recommended modes' first-transmission success
inline constexpr double BROADCAST_S = arq::SIZE_S.back();
std::string_view broadcast_mode(int cap);  // BROADCAST; throws std::out_of_range
void check_mode(const std::string& mode, int cap);  // throws std::invalid_argument unless a mode within the cap

struct Ax25 {
    std::string dst, src;
    std::string next_hop;  // who hears it next on RF
    std::string sender;    // who puts it on RF
    bool connected;        // I, S, or a U frame other than UI
};
std::optional<Ax25> parse_ax25(ByteView frame);  // nullopt: not AX.25
int station_hash(std::string_view call);         // nonzero 16-bit FNV-1a

// Groups and control. Each throws std::invalid_argument on what won't pack or parse.
std::string group_name(const std::string& group);  // as it reads back from the air
int group_key(const std::string& group);           // nonzero 16-bit FNV-1a of the packed name
Bytes pack_pair(const std::string& group, const std::string& call);  // T_GROUP_FROM, 15 bytes
std::pair<std::string, std::string> unpack_pair(ByteView b);
std::map<int, Bytes> parse_tlvs(ByteView b, bool partial = false);  // up to a zero type (padding); partial: the whole ones before a truncated one
struct ControlRead {
    std::string group;
    std::optional<std::string> call;   // T_GROUP_FROM's sender
    std::optional<Bytes> reports;      // T_REPORTS
};
ControlRead read_control(const std::map<int, Bytes>& tlvs);

// The most bytes an n-byte frame takes in a burst's data (COBS, then its ending zero).
inline std::size_t framed_len(std::size_t n) { return n + 2 + n / 254; }
Bytes cobs(ByteView frame);                   // Consistent Overhead Byte Stuffing: no zero byte left
std::optional<Bytes> uncobs(ByteView b);      // nullopt: not one cobs() makes
// A broadcast data codeword's identity: its slot, and the slot its burst's data starts at.
inline arq::MaskId data_start_mask(int slot, int start, int key) { return arq::data_mask(0, slot, key, false, start); }
// First data slots a burst may have: 1-4 control codewords, each sent once or twice (DUP).
inline constexpr int DATA_STARTS[] = {1, 2, 3, 4, 6, 8};
inline constexpr int PROBE_SLOTS = 8;  // control lost: data looked for in slots 1-8

// Data codewords: each frame COBS-encoded and ended by a zero, back to back, zero-padded; a frame that would start
// a codeword (the data's first included) gets a zero before it.
std::vector<Bytes> pack_frames(const std::vector<Bytes>& frames, std::size_t pb);
// Payloads and their CRC results -> (whole frames, frames lost). A zero byte
// in a good codeword always ends a frame, so parsing picks up again after a
// failed codeword; lost counts each run of frames one touched as one.
std::pair<std::vector<Bytes>, int> unpack_frames(const std::vector<Bytes>& payloads, const std::vector<bool>& ok);

struct Peer {
    arq::GearShifter shifter;
    double heard = 0.0;                             // when we last heard it
    std::optional<std::pair<int, double>> report;  // (rec byte, time): how it wants us to send to it
};

struct Port {
    std::string group;  // as it reads back (group_name)
    std::string mode;   // the transmit mode; with shifting on, the fallback
    bool shift = false;  // rate shifting (BCAST MODE n AUTO mode)
    std::optional<std::string> from_call;  // sent in T_GROUP_FROM
    bool dup = false;  // each control codeword sent twice (BCAST MODE n DUP mode)
    int key() const { return group_key(group); }
};

struct Queued {
    int port;
    Bytes frame;
    std::optional<std::int64_t> ack;  // opaque to the link: back in `acks` once sent
};

class KissLink {
public:
    // broadcast empty: broadcast_mode(cap). Throws std::invalid_argument for a
    // broadcast mode outside the cap.
    explicit KissLink(int cap = 2, std::string broadcast = {}, arq::Clock clock = arq::monotonic);

    int cap;
    std::vector<Queued> queue;                    // frames waiting
    std::vector<std::pair<int, Peer>> peers;     // station hash -> Peer, first heard first
    std::set<int> me;                             // hashes this TNC has sent as
    arq::Clock clock;
    std::int64_t n_sent = 0;
    std::string broadcast;  // a new port's transmit mode
    // channel access (the engine applies them)
    int persist = 63;  // after BUSY, a slot is taken with probability (P + 1) / 256
    double slot_s = SLOT_S;
    double busy_limit_s = 60.0;  // BUSY held a burst this long: send anyway
    std::map<int, Port> ports;                       // port number -> Port; 0 always
    std::vector<std::string> events;                 // statuses for the host, oldest first
    std::vector<std::pair<int, std::int64_t>> acks;  // (port, ack) of frames that went out

    // KISS P, SLOTTIME, and SETHARDWARE "MODE [AUTO] [DUP] mode" (set_mode on
    // `port`; bad ones logged); the rest ignored.
    void command(int cmd, ByteView payload, int port = 0);
    static constexpr int SETHARDWARE = 6;
    // Ports (the host's BCAST commands); each throws std::invalid_argument.
    int open(const std::string& group, const std::optional<std::string>& from_call = std::nullopt);
    void close(int n);  // frames queued for it are dropped (DROPPED)
    void set_mode(int n, const std::string& mode, bool shift = false, bool dup = false);
    // Ports as a new link has them: 1-15 closed (DROPPED, in port order), port 0 in the broadcast mode.
    void reset();

    // A frame for `port`; to a closed port: DROPPED.
    void enqueue(Bytes frame, int port = 0, std::optional<std::int64_t> ack = std::nullopt);
    std::vector<std::string> take_events();                  // the statuses since the last call
    std::vector<std::pair<int, std::int64_t>> take_acks();   // (port, ack) gone out since the last call
    // A burst finished transmitting: its acks are due. -> those (port, ack), which the engine also
    // reads (a HAND LISTEN's anchor).
    std::vector<std::pair<int, std::int64_t>> on_sent(const arq::TxBurstPtr& burst);
    // BCAST * MISSED; snr: its in-band energy reading (snr_est's scale), if one was made
    void missed(const std::string& submode, int n_cw, std::optional<double> snr = std::nullopt);
    // The first queued frame's port and mode, with every queued frame of that
    // port that goes in it, as many as fit. nullptr: nothing queued.
    arq::TxBurstPtr next_burst();
    // A received burst -> (port, frame)s for the open ports it belongs to
    // (empty for another group's), nullopt if it isn't a broadcast burst.
    // rx: the burst's decoder, shared with the engine's other checks.
    std::optional<std::vector<std::pair<int, Bytes>>> on_burst(const arq::Heard& r, arq::ModemRx& rx);
    // As above with a decoder of its own. soft: arq::soft_bits(r), if made.
    // dd_budget: as the engine's (never DD unbounded; nullopt only for tests).
    std::optional<std::vector<std::pair<int, Bytes>>> on_burst(const arq::Heard& r,
                                                               std::shared_ptr<const arq::SlotSoft> soft = nullptr,
                                                               std::optional<double> dd_budget = arq::DD_BUDGET_S);

    // A burst's broadcast control, checked under the key of the group it
    // names -> (control, the first data slot); nullopt: not a broadcast burst.
    // Codeword 0 is read alone, then with its RV 1 copy (DUP) when alone it
    // didn't decode (OFDM: no candidate converged; CPM: two control slots).
    static std::optional<std::tuple<ControlRead, int>> read_burst_control(arq::PlainRx& rx, int n_cw);
    // Control lost: the first of slots 1-8 passing as data under one of
    // `keys`, its mask naming where the data starts -> (key, first data slot).
    // A slot alone counts only as slot 1 starting the data, a burst's only
    // data slot, or at the start CPM's header gives; elsewhere a later slot
    // must pass under the same key and start too. Stops at a slot that decoded
    // to a codeword that is neither an open port's data nor the rest of its control.
    static std::optional<std::pair<int, int>> find_data(arq::PlainRx& rx, const std::set<int>& keys);
    // Its data slots from `start` under `key` -> (frames, per-slot ok, frames lost).
    static std::tuple<std::vector<Bytes>, std::vector<bool>, int> read_burst_frames(arq::PlainRx& rx, int start, int key);

private:
    void check_fits(int n, const Port& p, const std::string& mode, bool shift) const;
    void drop_where(int port, const std::function<bool(const Queued&)>& which);
    std::pair<std::string, int> route(const Port& port, ByteView frame) const;
    std::vector<Bytes> control(const arq::Mode& m, int n, const Port& port, int sender);
    void learn(const Bytes& rep, const std::vector<Bytes>& frames, const arq::Heard& r, const std::vector<bool>& ok,
               double now);
    Peer* peer(int h);

    arq::TxBurstPtr inflight_;  // the last burst handed out
    std::vector<std::pair<int, std::int64_t>> inflight_acks_;
};

}  // namespace data2g::kisslink
